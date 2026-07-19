#include "graph_builder.h"

#include <unordered_set>

namespace graph
{

// Entry-point function names per language. Functions matching these are
// call-graph roots (program entry / FFI exports) and must be flagged
// is_entry_point so downstream consumers (dead_code_inspector,
// state_builder) treat them as reachable roots instead of orphans.
// Go's `init` is an implicit entry point executed at package load.
// [module=graph, method=isEntryPointName]
static bool isEntryPointName(const std::string &name,
			     const std::string &language)
{
	if (name == "main") {
		return language == "c" || language == "cpp" ||
		       language == "c++" || language == "go" ||
		       language == "rust";
	}
	if (name == "init" && language == "go")
		return true;
	return false;
}

GraphBuilder::GraphBuilder(uint64_t project_id, uint64_t start_node_id)
	: project_id_(project_id)
	, next_node_id_(start_node_id)
{
}

CodeGraph GraphBuilder::buildSymbolGraph(ir::TranslationUnit *unit)
{
	current_graph_ = CodeGraph{};
	current_graph_.graph_type = "symbol_reference";
	current_graph_.name = "symbol-graph";
	next_edge_id_ = 1;
	ir_to_graph_node_.clear();
	function_stack_.clear();
	added_edges_.clear();
	building_call_graph_ = false;

	traverse(unit);
	return current_graph_;
}

CodeGraph GraphBuilder::buildCallGraph(ir::TranslationUnit *unit)
{
	current_graph_ = CodeGraph{};
	current_graph_.graph_type = "call_graph";
	current_graph_.name = "call-graph";
	next_edge_id_ = 1;
	added_edges_.clear();
	// NOTE: ir_to_graph_node_ and function_stack_ are intentionally NOT cleared
	// here. buildSymbolGraph already populated them with node ID mappings. By
	// keeping them, call edges reference the SAME graph node IDs, so the
	// SQL JOIN in getCallers/getCallees finds matching rows in graph_nodes.
	building_call_graph_ = true;

	traverse(unit);
	return current_graph_;
}

uint64_t GraphBuilder::getContainingFunctionNode()
{
	for (auto it = function_stack_.rbegin(); it != function_stack_.rend();
	     ++it) {
		auto found = ir_to_graph_node_.find((*it)->id);
		if (found != ir_to_graph_node_.end())
			return found->second;
	}
	return 0;
}

bool GraphBuilder::visitEnter(ir::Node *node)
{
	bool isFunction = (node->kind == ir::NodeKind::FunctionDecl ||
			   node->kind == ir::NodeKind::MethodDecl);

	if (isFunction) {
		function_stack_.push_back(node);
	}

	// ── Declarations → GraphNode ────────────────────────────
	switch (node->kind) {
	case ir::NodeKind::FunctionDecl:
		addGraphNode(node, NodeType::Function);
		break;
	case ir::NodeKind::MethodDecl:
		addGraphNode(node, NodeType::Method);
		break;
	case ir::NodeKind::ClassDecl:
		addGraphNode(node, NodeType::Class);
		break;
	case ir::NodeKind::EnumDecl:
		addGraphNode(node, NodeType::Module);
		break;
	case ir::NodeKind::VariableDecl:
		addGraphNode(node, NodeType::Variable);
		break;
	case ir::NodeKind::MacroDecl:
		addGraphNode(node, NodeType::Macro);
		break;
	case ir::NodeKind::Module:
	case ir::NodeKind::NamespaceDecl:
		addGraphNode(node, NodeType::Module);
		break;
	case ir::NodeKind::ImportDecl:
	case ir::NodeKind::ExportDecl:
		addGraphNode(node, NodeType::Module);
		break;
	case ir::NodeKind::TranslationUnit:
		addGraphNode(node, NodeType::File);
		break;
	default:
		break;
	}

	// ── Contains edges: parent → child hierarchy ────────────
	// If the parent node has a graph node, connect parent → child.
	// This builds the contains tree: file→class→method, etc.
	// We check the parent by looking at the function_stack for the
	// containing function, and the ir_to_graph_node_ for all ancestors.
	// For simplicity, use a container_stack_ that tracks which graph
	// node is the current container.
	if (!building_call_graph_) {
		// The current graph node (if any) is the child.
		// Its container is the nearest ancestor that has a graph node.
		auto child_it = ir_to_graph_node_.find(node->id);
		if (child_it != ir_to_graph_node_.end()) {
			// Find the container: the nearest ancestor in the IR tree
			// that also has a graph node. Since we visit in DFS order,
			// and the function stack tracks function ancestors, we look
			// there first, then fall back to the file-level parent.
			uint64_t container_id = getContainingFunctionNode();
			if (container_id == 0) {
				// Not inside a function — check if the parent IR node exists
				// (the File/Module level container). For simplicity, use
				// the File node if it exists.
				for (auto it = function_stack_.rbegin();
				     it != function_stack_.rend(); ++it) {
					auto found = ir_to_graph_node_.find(
						(*it)->id);
					if (found != ir_to_graph_node_.end()) {
						container_id = found->second;
						break;
					}
				}
			}
			if (container_id > 0 &&
			    container_id != child_it->second) {
				addGraphEdge(container_id, child_it->second,
					     EdgeType::Contains);
			}
		}
	}

	// ── Process semantic edges ──────────────────────────────
	for (auto &edge : node->semantic_edges) {
		// Skip CallTarget edges during symbol graph build.
		// They are only added during the call graph phase
		// (building_call_graph_ = true). Processing them during
		// the symbol phase creates false-positive call edges with
		// graph_type='symbol_reference' (e.g., a URL parameter
		// name like "agentId" being matched to a function "getAgent").
		if (!building_call_graph_ &&
		    edge.relation == ir::Relation::CallTarget)
			continue;

		// Determine the effective source graph node
		uint64_t effective_source = 0;

		if (edge.relation == ir::Relation::CallTarget) {
			// For call edges, source = containing function (caller), not the CallExpr
			effective_source = getContainingFunctionNode();
		} else if (edge.relation == ir::Relation::SymbolRef) {
			// SymbolRef on expression nodes (IdentifierExpr, CallExpr) not in graph:
			// use containing function as the referencing source
			auto it = ir_to_graph_node_.find(node->id);
			if (it != ir_to_graph_node_.end()) {
				effective_source = it->second;
			} else {
				effective_source = getContainingFunctionNode();
			}
		} else {
			auto it = ir_to_graph_node_.find(node->id);
			if (it != ir_to_graph_node_.end())
				effective_source = it->second;
		}

		if (effective_source == 0)
			continue;

		// Ensure target node exists in graph
		// Defensive: all current translators provide a valid target, but
		// a null target would cause a segfault on the dereference below.
		if (!edge.target)
			continue;
		auto it_tgt = ir_to_graph_node_.find(edge.target->id);
		if (it_tgt == ir_to_graph_node_.end()) {
			switch (edge.target->kind) {
			case ir::NodeKind::FunctionDecl:
				addGraphNode(edge.target, NodeType::Function);
				break;
			case ir::NodeKind::MethodDecl:
				addGraphNode(edge.target, NodeType::Method);
				break;
			case ir::NodeKind::ClassDecl:
				addGraphNode(edge.target, NodeType::Class);
				break;
			case ir::NodeKind::VariableDecl:
				addGraphNode(edge.target, NodeType::Variable);
				break;
			case ir::NodeKind::MacroDecl:
				addGraphNode(edge.target, NodeType::Macro);
				break;
			case ir::NodeKind::Module:
				addGraphNode(edge.target, NodeType::Module);
				break;
			default:
				continue;
			}
			it_tgt = ir_to_graph_node_.find(edge.target->id);
			if (it_tgt == ir_to_graph_node_.end())
				continue;
		}

		switch (edge.relation) {
		case ir::Relation::SymbolRef:
			if (!building_call_graph_)
				addGraphEdge(effective_source, it_tgt->second,
					     EdgeType::References);
			break;
		case ir::Relation::CallTarget:
			if (building_call_graph_)
				addGraphEdge(effective_source, it_tgt->second,
					     EdgeType::Calls);
			break;
		case ir::Relation::Receiver:
			if (!building_call_graph_)
				addGraphEdge(effective_source, it_tgt->second,
					     EdgeType::Contains);
			break;
		case ir::Relation::BaseClass:
			if (!building_call_graph_)
				addGraphEdge(effective_source, it_tgt->second,
					     EdgeType::Inherits);
			break;
		default:
			break;
		}
	}

	return true;
}

void GraphBuilder::addGraphNode(const ir::Node *ir_node, NodeType type)
{
	if (ir_to_graph_node_.count(ir_node->id))
		return;

	GraphNode gn;
	gn.id = next_node_id_++;
	gn.ir_node_id = ir_node->id;
	gn.type = type;
	gn.name = ir_node->name;
	gn.qualified_name = ir_node->qualified_name;
	gn.file_path = ir_node->file_path;
	gn.start_row = ir_node->loc.start_row;
	gn.start_col = ir_node->loc.start_col;
	gn.end_row = ir_node->loc.end_row;
	gn.end_col = ir_node->loc.end_col;
	gn.language = ir_node->language;

	// Flag call-graph roots (entry points) so dead_code_inspector and
	// state_builder can exclude them from orphan/dead classification.
	// The is_entry_point column previously defaulted to 0 everywhere,
	// so entry-point detection never fired. [module=graph,
	// method=addGraphNode]
	if ((type == NodeType::Function || type == NodeType::Method) &&
	    isEntryPointName(gn.name, gn.language)) {
		gn.is_entry_point = true;
	}

	ir_to_graph_node_[ir_node->id] = gn.id;
	current_graph_.nodes.push_back(std::move(gn));
}

void GraphBuilder::addGraphEdge(uint64_t src, uint64_t tgt, EdgeType type)
{
	// Dedup by (src, tgt, type, graph_type). Without this, the same edge
	// can be pushed many times (e.g. multiple CallExpr records resolving
	// to the same callee), and the DB has no unique constraint to filter
	// them at INSERT time. The set is cleared at the start of each build.
	auto key = std::make_tuple(src, tgt, type, current_graph_.graph_type);
	if (!added_edges_.insert(key).second)
		return; // duplicate edge — skip

	GraphEdge ge;
	ge.id = next_edge_id_++;
	ge.source_id = src;
	ge.target_id = tgt;
	ge.type = type;
	ge.graph_type = current_graph_.graph_type;
	current_graph_.edges.push_back(std::move(ge));
}

// ── SemanticUnit API ──────────────────────────────────────────

CodeGraph GraphBuilder::buildSymbolGraph(const ir::SemanticUnit &unit)
{
	current_graph_ = CodeGraph{};
	current_graph_.graph_type = "symbol_reference";
	current_graph_.name = "symbol-graph";
	next_edge_id_ = 1;
	ir_to_graph_node_.clear();
	function_stack_.clear();
	parent_cache_.clear();
	added_edges_.clear();
	building_call_graph_ = false;

	// Pass 1: Create GraphNode for each declaration record
	for (auto &rec : unit.allRecords()) {
		if (!isDeclarationKind(rec.kind))
			continue;
		NodeType nt = recordKindToNodeType(rec.kind);
		if (nt == NodeType::File &&
		    rec.kind == ir::RecordKind::Variable)
			continue; // skip anonymous root Variable (used as container)
		addGraphNode(rec, nt);
	}

	// Pass 2: Create Contains edges from parent_id (with caching)
	for (auto &rec : unit.allRecords()) {
		if (rec.parent_id == 0)
			continue;
		auto child_it = ir_to_graph_node_.find(rec.id);
		if (child_it == ir_to_graph_node_.end())
			continue;

		uint64_t ancestor_id = findContainingFunction(unit, rec);
		if (ancestor_id != 0 && ancestor_id != rec.id) {
			auto ancestor_it = ir_to_graph_node_.find(ancestor_id);
			if (ancestor_it != ir_to_graph_node_.end() &&
			    ancestor_it->second != child_it->second)
				addGraphEdge(ancestor_it->second,
					     child_it->second,
					     EdgeType::Contains);
		}
	}

	return current_graph_;
}

/**
 * Build a call graph from a SemanticUnit using layered callee resolution.
 *
 * Matching strategy (most precise first):
 *   1. ref_original_id: If a CallExpr record has ref_original_id != 0, look
 *      up the callee directly by that ID in ir_to_graph_node_. This is the
 *      most precise resolution (intra-file, resolver-assigned).
 *   2. qualified_name: If the CallExpr has a non-empty qualified_name, match
 *      candidates by their qualified_name (e.g., "MyClass::process").
 *   3. name + arity: Match candidates with the same bare name AND the same
 *      arity. This eliminates false edges between init() and init(int).
 *   4. name only (fallback): If the CallExpr's arity is 0 (unknown), fall
 *      back to matching all candidates with the same bare name.
 *
 * Arity rule: Never match across different arities when BOTH the CallExpr
 * and the candidate have known (non-zero) and different arities.
 *
 * Known limitations:
 *   - No control-flow graph (CFG) — call order and reachability not modeled.
 *   - No virtual dispatch resolution — virtual/abstract method calls connect
 *     to the statically-named target, not to all override implementations.
 *   - No function pointer / callback resolution — indirect calls are not
 *     resolved.
 *   - Cross-file calls may be unresolved when ref_original_id == 0 and the
 *     callee lives in another translation unit not present in the index.
 *   - Edges created by the name-only fallback (strategy 4) are approximate
 *     and may include false positives when multiple same-name functions exist.
 */
CodeGraph GraphBuilder::buildCallGraph(const ir::SemanticUnit &unit)
{
	// Local index: resolve calls against declarations in this same unit.
	CalleeIndex index = buildNameIndex(unit);
	return buildCallGraphImpl(unit, index);
}

CodeGraph GraphBuilder::buildCallGraph(const ir::SemanticUnit &unit,
				       const CalleeIndex &external_index)
{
	// External index: resolve calls against declarations from other units.
	return buildCallGraphImpl(unit, external_index);
}

CodeGraph GraphBuilder::buildCallGraphImpl(const ir::SemanticUnit &unit,
					   const CalleeIndex &index)
{
	current_graph_ = CodeGraph{};
	current_graph_.graph_type = "call_graph";
	current_graph_.name = "call-graph";
	next_edge_id_ = 1;
	added_edges_.clear();
	// Keep ir_to_graph_node_ from buildSymbolGraph — same node IDs
	building_call_graph_ = true;

	for (const auto &rec : unit.allRecords()) {
		if (rec.kind != ir::RecordKind::CallExpr)
			continue;
		// Skip calls with no resolvable signal at all
		if (rec.name.empty() && rec.qualified_name.empty() &&
		    rec.ref_original_id == 0)
			continue;

		// Find the containing function (caller)
		uint64_t caller_id = findContainingFunction(unit, rec);
		auto caller_it = ir_to_graph_node_.find(caller_id);
		if (caller_it == ir_to_graph_node_.end())
			continue;

		resolveCallEdges(rec, caller_it->second, index);
	}

	return current_graph_;
}

void GraphBuilder::resolveCallEdges(const ir::Record &call_rec,
				    uint64_t caller_graph_id,
				    const CalleeIndex &index)
{
	// ── Layer 1: ref_original_id (most precise) ───────────────
	// Direct resolved reference — skip all name-based matching.
	if (call_rec.ref_original_id != 0) {
		auto it = ir_to_graph_node_.find(call_rec.ref_original_id);
		if (it != ir_to_graph_node_.end() &&
		    it->second != caller_graph_id)
			addGraphEdge(caller_graph_id, it->second,
				     EdgeType::Calls);
		return;
	}

	// ── Layer 2: qualified_name match ─────────────────────────
	if (!call_rec.qualified_name.empty()) {
		auto [qbegin, qend] = index.by_qualified_name.equal_range(
			call_rec.qualified_name);
		bool matched = false;
		for (auto it = qbegin; it != qend; ++it) {
			if (it->second == caller_graph_id)
				continue; // skip self-call
			addGraphEdge(caller_graph_id, it->second,
				     EdgeType::Calls);
			matched = true;
		}
		if (matched)
			return; // qualified_name is precise — don't fall back to name
		// No qualified_name match — fall through to name+arity
	}

	// ── Layer 3 & 4: name + arity matching ────────────────────
	if (call_rec.name.empty())
		return; // no bare name to match on

	auto [nbegin, nend] = index.by_name.equal_range(call_rec.name);

	if (call_rec.arity != 0) {
		// CallExpr has known arity: prefer exact arity match, fall back to
		// candidates with unknown arity (0). Never match known-different arity.
		std::vector<uint64_t> exact_matches;
		std::vector<uint64_t> unknown_arity_matches;
		for (auto it = nbegin; it != nend; ++it) {
			uint64_t cand_id = it->second.graph_node_id;
			if (cand_id == caller_graph_id)
				continue; // skip self-call
			int cand_arity = it->second.arity;
			if (cand_arity == call_rec.arity)
				exact_matches.push_back(cand_id);
			else if (cand_arity == 0)
				unknown_arity_matches.push_back(cand_id);
			// else: known, different arity -> skip (arity rule)
		}
		// Prefer exact arity matches; fall back to unknown-arity candidates
		const std::vector<uint64_t> &chosen =
			exact_matches.empty() ? unknown_arity_matches :
						exact_matches;
		for (uint64_t tgt : chosen)
			addGraphEdge(caller_graph_id, tgt, EdgeType::Calls);
	} else {
		// CallExpr arity unknown (0): name-only fallback (approximate).
		for (auto it = nbegin; it != nend; ++it) {
			if (it->second.graph_node_id == caller_graph_id)
				continue; // skip self-call
			addGraphEdge(caller_graph_id, it->second.graph_node_id,
				     EdgeType::Calls);
		}
	}
}

// ─── Type Edge Builder ─────────────────────────────────────────

void GraphBuilder::buildTypeEdges(const ir::SemanticUnit &unit)
{
	// Build a name → graph_node_id index for TypeDecl records once,
	// avoiding O(n²) nested loop for each TypeRef/TypeAssign lookup.
	// Reference: codebase-memory-mcp (MIT) extract_type_refs.c
	std::unordered_map<std::string, uint64_t> type_decl_map;
	for (auto &other : unit.allRecords()) {
		if (other.kind == ir::RecordKind::TypeDecl &&
		    !other.name.empty()) {
			auto it = ir_to_graph_node_.find(other.id);
			if (it != ir_to_graph_node_.end())
				type_decl_map[other.name] = it->second;
		}
	}

	// Iterate TypeRef and TypeAssign records, creating USES_TYPE edges
	// from the entity that references a type to its type declaration.
	for (auto &rec : unit.allRecords()) {
		if (rec.kind != ir::RecordKind::TypeRef &&
		    rec.kind != ir::RecordKind::TypeAssign)
			continue;
		if (rec.name.empty() || rec.type_name.empty())
			continue;
		auto src_it = ir_to_graph_node_.find(rec.id);
		if (src_it == ir_to_graph_node_.end())
			continue;

		// O(1) lookup via the pre-built index
		auto tgt_it = type_decl_map.find(rec.type_name);
		if (tgt_it == type_decl_map.end())
			continue;

		addGraphEdge(src_it->second, tgt_it->second,
			     EdgeType::UsesType);
	}
}

// ── Helpers ──────────────────────────────────────────────────

NodeType GraphBuilder::recordKindToNodeType(ir::RecordKind kind)
{
	switch (kind) {
	case ir::RecordKind::Function:
		return NodeType::Function;
	case ir::RecordKind::Method:
		return NodeType::Method;
	case ir::RecordKind::Class:
		return NodeType::Class;
	case ir::RecordKind::Interface:
		return NodeType::Interface;
	case ir::RecordKind::Enum:
		return NodeType::Module;
	case ir::RecordKind::TypeAlias:
		return NodeType::Module;
	case ir::RecordKind::TypeDecl:
		return NodeType::Module;
	case ir::RecordKind::Variable:
		return NodeType::Variable;
	case ir::RecordKind::Import:
		return NodeType::Module;
	case ir::RecordKind::Export:
		return NodeType::Module;
	case ir::RecordKind::Field:
		return NodeType::Variable;
	default:
		return NodeType::File; // sentinel
	}
}

bool GraphBuilder::isDeclarationKind(ir::RecordKind kind)
{
	switch (kind) {
	case ir::RecordKind::Function:
	case ir::RecordKind::Method:
	case ir::RecordKind::Class:
	case ir::RecordKind::Interface:
	case ir::RecordKind::Enum:
	case ir::RecordKind::TypeAlias:
	case ir::RecordKind::TypeDecl:
	case ir::RecordKind::Variable:
	case ir::RecordKind::Field:
	case ir::RecordKind::Import:
	case ir::RecordKind::Export:
		return true;
	default:
		return false;
	}
}

void GraphBuilder::addGraphNode(const ir::Record &rec, NodeType type)
{
	if (ir_to_graph_node_.count(rec.id))
		return;

	GraphNode gn;
	gn.id = next_node_id_++;
	gn.ir_node_id = rec.id;
	gn.type = type;
	gn.name = rec.name;
	gn.qualified_name = rec.qualified_name.empty() ? rec.name :
							 rec.qualified_name;
	gn.file_path = rec.file_path;
	gn.start_row = rec.loc.start_row;
	gn.start_col = rec.loc.start_col;
	gn.end_row = rec.loc.end_row;
	gn.end_col = rec.loc.end_col;
	gn.language = rec.language;

	ir_to_graph_node_[rec.id] = gn.id;
	if (rec.original_id != 0 && rec.original_id != rec.id)
		ir_to_graph_node_[rec.original_id] = gn.id;
	current_graph_.nodes.push_back(std::move(gn));
}

uint64_t GraphBuilder::findContainingFunction(const ir::SemanticUnit &unit,
					      const ir::Record &rec) const
{
	uint64_t pid = rec.parent_id;
	if (pid == 0)
		return 0;

	// Check parent cache first
	{
		auto cache_it = parent_cache_.find(pid);
		if (cache_it != parent_cache_.end())
			return cache_it->second;
	}

	// Walk parent_id chain until we find a node that has a graph node.
	// Cache each step so subsequent lookups skip the walk.
	// A visited set guards against cycles in malformed IR (parent_id loop),
	// which would otherwise hang this loop indefinitely. getRecord now
	// returns nullptr for unknown ids, so we break the chain on miss
	// instead of dereferencing a sentinel record.
	std::unordered_set<uint64_t> visited;
	uint64_t result = 0;
	while (pid != 0) {
		if (!visited.insert(pid).second)
			break; // cycle detected — stop walking
		auto it = ir_to_graph_node_.find(pid);
		if (it != ir_to_graph_node_.end()) {
			result = pid;
			break;
		}
		// Check cache along the way
		auto cache_it = parent_cache_.find(pid);
		if (cache_it != parent_cache_.end()) {
			result = cache_it->second;
			break;
		}
		const auto *parent_rec = unit.getRecord(pid);
		if (!parent_rec)
			break; // id not found — stop walking
		pid = parent_rec->parent_id;
	}

	// Cache the result for the starting pid so future lookups skip the walk
	parent_cache_[rec.parent_id] = result;
	return result;
}

CalleeIndex GraphBuilder::buildNameIndex(const ir::SemanticUnit &unit) const
{
	return buildCalleeIndex(unit, ir_to_graph_node_);
}

CalleeIndex GraphBuilder::buildCalleeIndex(
	const ir::SemanticUnit &unit,
	const std::unordered_map<uint64_t, uint64_t> &ir_to_graph_node)
{
	CalleeIndex idx;
	// Iterate SemanticUnit records and look up their graph node IDs
	// from ir_to_graph_node (preserved from buildSymbolGraph).
	for (const auto &rec : unit.allRecords()) {
		auto it = ir_to_graph_node.find(rec.id);
		if (it == ir_to_graph_node.end())
			continue; // no graph node for this record
		if (rec.name.empty())
			continue;

		// Index by qualified_name (when available) for precise lookup
		if (!rec.qualified_name.empty())
			idx.by_qualified_name.emplace(rec.qualified_name,
						      it->second);

		// Index by name + arity for layered name matching
		idx.by_name.emplace(rec.name,
				    CalleeCandidate{ rec.arity, it->second });
	}
	return idx;
}

} // namespace graph
