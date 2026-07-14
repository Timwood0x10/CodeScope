// engine_index_metrics.cpp — Metric computation helpers for the indexer.
//
// Extracted from engine_index_project.cpp to keep that file under the
// 1000-line limit imposed by plan/rules/code_rules.md §1.

#include "engine_index_metrics.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace index_metrics
{

// ─── computeMetricsFromCST ─────────────────────────────────────────

std::vector<store::MetricRow>
computeMetricsFromCST(TSTree *tree, const char *source,
		      const std::vector<ir::Record> &records)
{
	std::vector<store::MetricRow> result;
	if (!tree || !source || records.empty())
		return result;

	// Build line-start byte offset table for row→byte conversion
	std::vector<uint32_t> line_starts;
	line_starts.push_back(0);
	for (size_t i = 0; source[i]; ++i)
		if (source[i] == '\n')
			line_starts.push_back(static_cast<uint32_t>(i + 1));

	auto rowColToByte = [&](uint32_t row, uint32_t col) -> uint32_t {
		if (row >= line_starts.size())
			row = static_cast<uint32_t>(line_starts.size() - 1);
		return line_starts[row] + col;
	};

	// Build parent→children index and record map
	std::unordered_map<uint64_t, std::vector<uint64_t> > children_of;
	children_of.reserve(records.size());
	for (auto &r : records) {
		if (r.parent_id > 0)
			children_of[r.parent_id].push_back(r.id);
	}
	std::unordered_map<uint64_t, const ir::Record *> record_map;
	record_map.reserve(records.size());
	for (auto &r : records)
		record_map[r.id] = &r;

	// Collect Function/Method records (RecordKind: Function=0, Method=1)
	// with byte ranges, sorted by start_byte.
	struct FuncEntry {
		uint32_t start_byte;
		uint32_t end_byte;
		const ir::Record *rec;
	};
	std::vector<FuncEntry> funcs;
	funcs.reserve(records.size() / 4);
	for (auto &r : records) {
		if (r.kind != ir::RecordKind::Function &&
		    r.kind != ir::RecordKind::Method)
			continue;
		FuncEntry fe;
		fe.start_byte = rowColToByte(r.loc.start_row, r.loc.start_col);
		fe.end_byte = rowColToByte(r.loc.end_row, r.loc.end_col);
		fe.rec = &r;
		funcs.push_back(fe);
	}
	if (funcs.empty())
		return result;

	std::sort(funcs.begin(), funcs.end(),
		  [](const FuncEntry &a, const FuncEntry &b) {
			  return a.start_byte < b.start_byte;
		  });

	// Binary search: find innermost function containing byte_offset.
	// Returns the function with the largest start_byte <= offset
	// whose end_byte > offset. Walks backwards to handle nesting.
	auto findContainingFunc =
		[&](uint32_t byte_offset) -> const FuncEntry * {
		auto it = std::upper_bound(
			funcs.begin(), funcs.end(), byte_offset,
			[](uint32_t val, const FuncEntry &fe) {
				return val < fe.start_byte;
			});
		while (it != funcs.begin()) {
			--it;
			if (byte_offset >= it->start_byte &&
			    byte_offset < it->end_byte)
				return &(*it);
		}
		return nullptr;
	};

	// Control-flow node types (tree-sitter grammar strings,
	// covering JS/TS/Go/Rust/C/C++/Python/Java)
	static const std::unordered_set<std::string_view> branch_types = {
		"if_statement",
		"if_expression",
		"switch_statement",
		"switch_expression",
		"match_expression",
		"match_statement",
		"case",
		"case_clause",
		"case_statement",
		"catch_clause",
		"except_clause",
		"handler_clause",
		"conditional_expression",
		"ternary_expression",
		"select_statement"
	};
	static const std::unordered_set<std::string_view> loop_types = {
		"for_statement",    "for_expression",	  "for_in_statement",
		"for_of_statement", "while_statement",	  "while_expression",
		"do_statement",	    "do_while_statement", "loop_expression"
	};

	// Initialize MetricRow per function, counting params and calls
	// from records via an iterative DFS over the record subtree.
	// Using an explicit stack instead of std::function avoids a heap
	// allocation per recursive step (millions of allocations across
	// a large project).
	std::unordered_map<const ir::Record *, store::MetricRow> metrics_map;
	metrics_map.reserve(funcs.size());
	std::vector<uint64_t> desc_stack;
	for (auto &fe : funcs) {
		store::MetricRow m;
		m.name = fe.rec->name;
		m.line = static_cast<int>(fe.rec->loc.start_row);
		m.col = static_cast<int>(fe.rec->loc.start_col);
		m.lines = static_cast<int>(fe.rec->loc.end_row -
					   fe.rec->loc.start_row + 1);
		m.cyclomatic = 1;
		bool has_call = false;

		// Iterative DFS over the record subtree rooted at this
		// function. Order of traversal does not affect the counts
		// (they are sums), so a plain LIFO stack is equivalent.
		desc_stack.clear();
		auto ci = children_of.find(fe.rec->id);
		if (ci != children_of.end())
			for (auto cid : ci->second)
				desc_stack.push_back(cid);
		while (!desc_stack.empty()) {
			uint64_t id = desc_stack.back();
			desc_stack.pop_back();
			auto it = record_map.find(id);
			if (it == record_map.end())
				continue;
			const ir::Record *rec = it->second;
			if (rec->kind == ir::RecordKind::Parameter)
				m.param_count++;
			else if (rec->kind == ir::RecordKind::CallExpr) {
				m.call_count++;
				has_call = true;
			}
			auto ci2 = children_of.find(id);
			if (ci2 != children_of.end())
				for (auto cid : ci2->second)
					desc_stack.push_back(cid);
		}
		m.is_stub = !has_call;
		metrics_map[fe.rec] = std::move(m);
	}

	// Walk the CST iteratively with an explicit stack, counting
	// control-flow nodes per function. cf_depth tracks nesting of
	// control-flow nodes and resets when entering a different
	// function. Uses ts_node_child_count/ts_node_child (ALL children,
	// not just named ones) to match the original recursive traversal.
	// Metrics are sums and a max, so traversal order is irrelevant to
	// the result; children are pushed in reverse so they pop
	// left-to-right, preserving the original visitation order.
	struct WalkFrame {
		TSNode node;
		int cf_depth;
		const FuncEntry *cur_func;
	};
	std::vector<WalkFrame> walk_stack;
	walk_stack.push_back({ ts_tree_root_node(tree), 0, nullptr });
	while (!walk_stack.empty()) {
		WalkFrame frame = walk_stack.back();
		walk_stack.pop_back();
		TSNode node = frame.node;
		int cf_depth = frame.cf_depth;
		const FuncEntry *cur_func = frame.cur_func;

		uint32_t start_byte = ts_node_start_byte(node);
		const FuncEntry *fe = findContainingFunc(start_byte);
		int eff_depth = cf_depth;
		const FuncEntry *eff_func = cur_func;
		if (fe && fe != cur_func) {
			eff_depth = 0;
			eff_func = fe;
		}

		const char *type = ts_node_type(node);
		std::string_view sv(type);
		bool is_branch = branch_types.count(sv) > 0;
		bool is_loop = loop_types.count(sv) > 0;

		if (eff_func && (is_branch || is_loop)) {
			if (is_branch)
				metrics_map[eff_func->rec].branch_count++;
			else
				metrics_map[eff_func->rec].loop_count++;
			eff_depth = eff_depth + 1;
			if (eff_depth >
			    metrics_map[eff_func->rec].nesting_depth)
				metrics_map[eff_func->rec].nesting_depth =
					eff_depth;
		}

		uint32_t n = ts_node_child_count(node);
		for (uint32_t i = n; i > 0; --i)
			walk_stack.push_back({ ts_node_child(node, i - 1),
					       eff_depth, eff_func });
	}

	// Finalize: cyclomatic = 1 + branches + loops,
	// cognitive = cyclomatic + nesting_depth (approximation)
	result.reserve(funcs.size());
	for (auto &fe : funcs) {
		auto &m = metrics_map[fe.rec];
		m.cyclomatic = 1 + m.branch_count + m.loop_count;
		m.cognitive = m.cyclomatic + m.nesting_depth;
		result.push_back(std::move(m));
	}
	return result;
}

// ─── computeMetricsFromUnit ────────────────────────────────────────

std::vector<store::MetricRow> computeMetricsFromUnit(ir::TranslationUnit *unit)
{
	std::vector<store::MetricRow> result;
	if (!unit)
		return result;

	for (auto *node : unit->all_nodes) {
		if (node->kind != ir::NodeKind::FunctionDecl &&
		    node->kind != ir::NodeKind::MethodDecl)
			continue;

		store::MetricRow m;
		m.name = node->name;
		m.line = static_cast<int>(node->loc.start_row);
		m.col = static_cast<int>(node->loc.start_col);
		m.lines = static_cast<int>(node->loc.end_row -
					   node->loc.start_row + 1);

		// Count params, calls, branches, loops
		std::function<void(ir::Node *)> count = [&](ir::Node *n) {
			switch (n->kind) {
			case ir::NodeKind::ParameterDecl:
				m.param_count++;
				break;
			case ir::NodeKind::CallExpr:
				m.call_count++;
				break;
			case ir::NodeKind::IfStmt:
			case ir::NodeKind::SwitchStmt:
			case ir::NodeKind::CaseStmt:
				m.branch_count++;
				break;
			case ir::NodeKind::ForStmt:
			case ir::NodeKind::WhileStmt:
			case ir::NodeKind::DoWhileStmt:
				m.loop_count++;
				break;
			default:
				break;
			}
			for (auto *c : n->children)
				count(c);
		};
		count(node);

		// Finalize: cyclomatic = 1 + branches + loops,
		// cognitive = cyclomatic + nesting_depth (approximation)
		m.cyclomatic = 1 + m.branch_count + m.loop_count;
		m.cognitive = m.cyclomatic + m.nesting_depth;

		// Stub detection
		bool has_real_stmt = false;
		std::function<void(ir::Node *)> stub_check = [&](ir::Node *n) {
			if (has_real_stmt)
				return;
			switch (n->kind) {
			case ir::NodeKind::CallExpr:
			case ir::NodeKind::IfStmt:
			case ir::NodeKind::ForStmt:
			case ir::NodeKind::WhileStmt:
			case ir::NodeKind::VariableDecl:
			case ir::NodeKind::TryStmt:
				has_real_stmt = true;
				return;
			default:
				break;
			}
			for (auto *c : n->children)
				stub_check(c);
		};
		stub_check(node);
		m.is_stub = !has_real_stmt;

		result.push_back(std::move(m));
	}
	return result;
}

} // namespace index_metrics
