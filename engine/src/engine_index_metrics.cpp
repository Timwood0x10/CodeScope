// engine_index_metrics.cpp — Metric computation helpers for the indexer.
//
// Extracted from engine_index_project.cpp to keep that file under the
// 1000-line limit imposed by plan/rules/code_rules.md §1.

#include "engine_index_metrics.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

/// Parameter counts of one function declaration, in the two forms the two
/// consumers need. `declared` is what the source spells out (the code metric);
/// `arity` is how many arguments a call site supplies, which is what the
/// Resolver compares against a reference row. They differ by an implicit
/// receiver: Python and Rust write it as a parameter (`self` / `&self`) while
/// the call syntax hides it, so a call site that supplies n arguments has
/// arity n for a method that declares n + 1 parameters.
struct ParameterCount {
	int declared = 0;
	int arity = 0;
};

/// True when the node IS a parameter list. The `parameters` field of a function
/// declaration holds one of these in every grammar this indexer parses;
/// anything else in that field is a single bare parameter, which JavaScript and
/// TypeScript allow (`x => x`).
bool isParameterListType(const char *type)
{
	return strcmp(type, "parameter_list") == 0 || // C, C++, Go
	       strcmp(type, "parameters") == 0 || // Rust, Python
	       strcmp(type, "formal_parameters") ==
		       0; // JavaScript, TypeScript, Java
}

/// Text of a node with whitespace, parentheses and comma removed — enough to
/// recognise C's `(void)`, whose single `parameter_declaration` is the one
/// parameter that declares no parameter. `(void *)` normalises to `void*` and
/// is therefore not mistaken for it.
std::string normalizedParameterText(TSNode node, const char *source)
{
	const uint32_t start = ts_node_start_byte(node);
	const std::string text(source + start, ts_node_end_byte(node) - start);
	std::string norm;
	norm.reserve(text.size());
	for (char c : text)
		if (c != '(' && c != ')' && c != ',' &&
		    !isspace(static_cast<unsigned char>(c)))
			norm.push_back(c);
	return norm;
}

/// The identifier a parameter starts with: `self` in `self: Node = None`,
/// `cls` in `cls`, `self` in `&self` (Rust spells it as a node type instead).
/// Stops at the first separator that ends a name.
std::string leadingParameterName(TSNode param, const char *source)
{
	const uint32_t start = ts_node_start_byte(param);
	const std::string text(source + start, ts_node_end_byte(param) - start);
	std::string name;
	for (char c : text) {
		if (c == ':' || c == '=' || c == '&' || c == '<' ||
		    isspace(static_cast<unsigned char>(c)))
			break;
		name.push_back(c);
	}
	return name;
}

/// True when the declaration sits inside a class body. Only Python needs this:
/// it is the one grammar here whose receiver is an ordinary parameter named by
/// convention, so the name alone would also drop a free function's first
/// argument (a function named `self` outside a class is not a method).
///
/// Cost: one ancestor walk, run only for a parameter whose name is
/// `self`/`cls`, and bounded by the tree depth — the walk's visit cap keeps
/// nesting finite, so this is invisible next to the CST traversal that called
/// it. If that cap is ever lifted, cache the verdict per declaration node
/// rather than walking the same ancestors once per parameter.
bool isMethodDeclaration(TSNode decl)
{
	for (TSNode n = ts_node_parent(decl); !ts_node_is_null(n);
	     n = ts_node_parent(n))
		if (strcmp(ts_node_type(n), "class_definition") == 0)
			return true;
	return false;
}

/// True when a parameter node type makes the number of arguments a call may
/// supply differ from the declared count: a variadic tail accepts any number of
/// extras and a default value lets a call supply fewer. `arity` is the count a
/// call site must supply, so such a declaration has no exact answer.
bool hasOpenParameterCount(const char *type)
{
	return strcmp(type, "variadic_parameter") == 0 || // C, C++ (…)
	       strcmp(type, "variadic_parameter_declaration") ==
		       0 || // Go (rest ...T)
	       strcmp(type, "list_splat_pattern") == 0 || // Python *args
	       strcmp(type, "dictionary_splat_pattern") == 0 || // Python **kw
	       strcmp(type, "rest_pattern") ==
		       0 || // JavaScript/TypeScript ...rest
	       strcmp(type, "default_parameter") == 0 || // Python (b=1)
	       strcmp(type, "typed_default_parameter") ==
		       0 || // Python (b: int = 1)
	       strcmp(type, "optional_parameter_declaration") ==
		       0 || // C++ (b = 0)
	       strcmp(type, "assignment_pattern") == 0; // JavaScript/TypeScript
}

/// True when the parameter's source text contains `...`, the one marker every
/// variadic spelling shares (`opts ...any`, `...rest`, `T...`) and that no other
/// parameter declaration contains.
bool textHasEllipsis(TSNode param, const char *source)
{
	const uint32_t start = ts_node_start_byte(param);
	const std::string text(source + start, ts_node_end_byte(param) - start);
	return text.find("...") != std::string::npos;
}

/// True when the parameter's source text assigns it a value (`bool append =
/// false`, `std::string name = "x"`).
///
/// C++ spells a default INSIDE the ordinary `parameter_declaration`; the
/// `optional_parameter_declaration` node the type list above expects is not what
/// tree-sitter-cpp produces for it. So `bool loadIgnoreFile(const std::string
/// &root, bool append = false)` was recorded with arity 2 while a legitimate
/// one-argument call - which the default makes valid - was then PENALISED by
/// factorSignatureMatch (arity 1 against 2 scores kScorePenalty), and the edge
/// disappeared. Measured on this repository by rebuilding the previous commit
/// and diffing call edges: `policyFor -> loadIgnoreFile`, `indexProjectImpl ->
/// loadIgnoreFile` / `loadGitignore` / `insertFileResultBatch` and every other
/// lost edge were calls that omit a defaulted argument. `=` cannot otherwise
/// appear in a parameter declaration: the function's own name is outside the
/// list (`bool operator==(const X &o)` has no `=` among the parameters).
bool textHasDefaultValue(TSNode param, const char *source)
{
	const uint32_t start = ts_node_start_byte(param);
	const std::string text(source + start, ts_node_end_byte(param) - start);
	return text.find('=') != std::string::npos;
}

/// Parameter counts of a parameter-list node: the number of NAMED children,
/// minus the two traps. `comment` is a named node in every grammar and is not a
/// parameter, and C's `(void)` is a single parameter_declaration that declares
/// none.
ParameterCount countParameters(TSNode params, TSNode decl, const char *source)
{
	ParameterCount out;
	if (!isParameterListType(ts_node_type(params))) {
		out.declared = out.arity = 1; // a bare parameter, e.g. `x => x`
		return out;
	}

	std::vector<TSNode> declared;
	bool open_count = false;
	for (uint32_t i = 0, n = ts_node_named_child_count(params); i < n;
	     ++i) {
		TSNode child = ts_node_named_child(params, i);
		const char *type = ts_node_type(child);
		if (strcmp(type, "comment") == 0)
			continue;
		if (hasOpenParameterCount(type) ||
		    textHasEllipsis(child, source) ||
		    textHasDefaultValue(child, source))
			open_count = true;
		declared.push_back(child);
	}
	if (declared.size() == 1 &&
	    normalizedParameterText(declared.front(), source) == "void")
		return out; // `(void)` — no parameter at all

	out.declared = static_cast<int>(declared.size());
	if (declared.empty())
		return out;

	// A variadic tail or a default value means a valid call may pass a
	// different number of arguments than the declaration lists (155 functions
	// in goagent alone are variadic), so the count a call site must supply is
	// unknown — 0 keeps the Resolver on its pre-existing behaviour for these
	// instead of penalising the call that matches them.
	if (open_count)
		return out;

	out.arity = out.declared;
	const TSNode first = declared.front();
	const std::string first_name = leadingParameterName(first, source);
	if (strcmp(ts_node_type(first), "self_parameter") == 0 ||
	    (isMethodDeclaration(decl) &&
	     (first_name == "self" || first_name == "cls")))
		out.arity -= 1; // the call site never passes the receiver
	return out;
}

/// The parameter list declared under a declaration node, for the grammars whose
/// declaration does not own the list itself: C and C++ put it on the nested
/// declarator (`function_definition` → `function_declarator`), so a direct field
/// lookup on the declaration finds nothing.
///
/// Bounded to two levels, which keeps the search inside the declaration: a
/// callback's parameter list (C's `void f(int (*cb)(int a))`) hangs off a
/// different declarator and a lambda in the body sits deeper than that, so
/// neither can be mistaken for the declaration's own parameters.
TSNode parameterListUnderDeclarator(TSNode decl)
{
	std::vector<TSNode> level{ decl };
	for (int depth = 0; depth < 2; ++depth) {
		std::vector<TSNode> next;
		for (TSNode n : level) {
			for (uint32_t i = 0,
				      n_children = ts_node_child_count(n);
			     i < n_children; ++i) {
				TSNode child = ts_node_child(n, i);
				TSNode params = ts_node_child_by_field_name(
					child, "parameters", 10);
				if (!ts_node_is_null(params))
					return params;
				next.push_back(child);
			}
		}
		level.swap(next);
	}
	return TSNode{};
}

/// Parameter counts of the function declaration owning the record spanning
/// [start_byte, end_byte). Returns false when the CST exposes no such
/// declaration (the legacy translator path, or a grammar whose declaration node
/// carries no `parameters` field), in which case the caller leaves the counts
/// at 0 — absent, not wrong.
///
/// Two lookups, because a record's range starts either at the declaration or at
/// the declared name:
///   * the record's own node may span the whole declaration, in which case the
///     list is a field of it (Go, Java, JavaScript, Python, Rust) or one level
///     down on its declarator (C, C++);
///   * or it spans only the name, in which case climbing to the nearest
///     ancestor carrying a `parameters` field finds that function's own list.
/// Either way the list belongs to this declaration: a Go method's receiver is a
/// separate field and is not counted as a parameter.
bool parameterCountForRecord(TSNode root, uint32_t start_byte,
			     uint32_t end_byte, const char *source,
			     ParameterCount &out)
{
	TSNode node = ts_node_descendant_for_byte_range(
		root, start_byte,
		end_byte > start_byte ? end_byte - 1 : start_byte);
	bool own_node = true;
	while (!ts_node_is_null(node)) {
		TSNode params =
			ts_node_child_by_field_name(node, "parameters", 10);
		if (ts_node_is_null(params) && own_node)
			params = parameterListUnderDeclarator(node);
		if (!ts_node_is_null(params)) {
			out = countParameters(params, node, source);
			return true;
		}
		own_node = false;
		node = ts_node_parent(node);
	}
	return false;
}

} // namespace

namespace index_metrics
{

// ─── computeMetricsFromCST ─────────────────────────────────────────

std::vector<store::MetricRow>
computeMetricsFromCST(TSTree *tree, const char *source,
		      std::vector<ir::Record> &records)
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
	std::unordered_map<uint64_t, std::vector<uint64_t>> children_of;
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
		ir::Record *rec;
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

	// Parameter counts come from the CST, not from the record subtree walked
	// above: RecordKind::Parameter (8) exists and the walk counts it, but no
	// Visitor ever emits one — the record layer carries parameters as
	// arguments of a declaration it does not model — so param_count was 0 on
	// every function of every project (goagent: 6163 function entities, 0 with
	// param_count > 0) while cyclomatic/lines/cognitive were correct. The
	// count is read from the function declaration's own `parameters` field.
	//
	// The same number is written to the record's `arity`: buildGraph fills
	// entity.arity from semantic_records.arity and the Resolver Pipeline reads
	// entity.arity to weigh same-name overloads (factorSignatureMatch), so
	// without a producer for it every candidate scored as "unknown arity" —
	// the factor could neither prefer the overload whose parameter count
	// matches the call site nor penalise the ones that do not.
	{
		const TSNode cst_root = ts_tree_root_node(tree);
		for (auto &fe : funcs) {
			ParameterCount params;
			if (!parameterCountForRecord(cst_root, fe.start_byte,
						     fe.end_byte, source,
						     params))
				continue; // no declaration to read — leave 0
			metrics_map[fe.rec].param_count = params.declared;
			fe.rec->arity = params.arity;
		}
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

	// Iterative DFS using an explicit stack. The original recursive
	// std::function implementation could overflow the call stack on
	// deeply nested ASTs (generated/minified code can reach 1000+
	// levels). The primary path (computeMetricsFromCST) was already
	// converted to iterative; this fallback path now matches.
	// A single pass performs both metric counting and stub detection
	// — the counts are sums so traversal order is irrelevant, and
	// stub detection only needs to know whether ANY real statement
	// exists in the subtree.
	std::vector<ir::Node *> dfs_stack;

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

		// Single iterative DFS: count params/calls/branches/loops AND
		// detect whether the body contains any real statement (for the
		// stub flag). Pushing children in natural order is fine since
		// the metrics are order-independent sums. The stub-flag kind
		// set is intentionally a subset of the counted kinds — this
		// preserves the exact behaviour of the original two separate
		// recursive lambdas (count vs stub_check).
		bool has_real_stmt = false;
		dfs_stack.clear();
		dfs_stack.push_back(node);
		while (!dfs_stack.empty()) {
			ir::Node *n = dfs_stack.back();
			dfs_stack.pop_back();
			// Counting pass (mirrors original `count` lambda).
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
			// Stub detection (mirrors original `stub_check` lambda).
			// Only a subset of kinds set has_real_stmt.
			if (!has_real_stmt) {
				switch (n->kind) {
				case ir::NodeKind::CallExpr:
				case ir::NodeKind::IfStmt:
				case ir::NodeKind::ForStmt:
				case ir::NodeKind::WhileStmt:
				case ir::NodeKind::VariableDecl:
				case ir::NodeKind::TryStmt:
					has_real_stmt = true;
					break;
				default:
					break;
				}
			}
			for (auto *c : n->children)
				dfs_stack.push_back(c);
		}

		// Finalize: cyclomatic = 1 + branches + loops,
		// cognitive = cyclomatic + nesting_depth (approximation)
		m.cyclomatic = 1 + m.branch_count + m.loop_count;
		m.cognitive = m.cyclomatic + m.nesting_depth;
		m.is_stub = !has_real_stmt;

		result.push_back(std::move(m));
	}
	return result;
}

} // namespace index_metrics
