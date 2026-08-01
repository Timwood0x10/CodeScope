#ifndef GRAPH_TYPES_H
#define GRAPH_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace graph
{

// ─── Node Types ────────────────────────────────────────────────

enum class NodeType : uint8_t {
	Function = 0,
	Method,
	Class,
	Struct,
	Interface,
	Variable,
	Macro,
	Module,
	File,
};

// ─── Edge Types ────────────────────────────────────────────────

enum class EdgeType : uint8_t {
	References = 0, // symbol reference
	Calls, // function call
	Defines, // defines a symbol
	Contains, // class contains method, module contains function
	Imports, // import/include
	Inherits, // class inheritance (v2)
	UsesType, // entity uses a type (variable : Type, param : Type, etc.)
	HasType, // type definition has fields/methods
};

// ─── Graph Node ────────────────────────────────────────────────

struct GraphNode {
	uint64_t id = 0;
	uint64_t ir_node_id = 0; // back-reference to IR record
	NodeType type;
	std::string name;
	std::string qualified_name;
	std::string module_path; // "src/network/http/" — derived from file_path
	std::string package_name; // "com.example.server"
	std::string class_name; // "Server" — empty = top-level function
	std::string file_path;
	uint32_t start_row = 0, start_col = 0;
	uint32_t end_row = 0, end_col = 0;
	std::string language;
	std::string signature; // "listen(port: number, host: string): void"

	bool is_entry_point = false;
};

// ─── Graph Edge ────────────────────────────────────────────────

struct GraphEdge {
	uint64_t id = 0;
	uint64_t source_id = 0; // GraphNode.id
	uint64_t target_id = 0; // GraphNode.id
	EdgeType type;
	std::string graph_type; // "call_graph" | "symbol_reference"
	std::string call_site_file; // file where the call happens
	int call_site_line = 0; // line number of the call
	std::string label; // "async" / "virtual" / "override"
};

// ─── Code Graph ────────────────────────────────────────────────

struct CodeGraph {
	uint64_t id = 0;
	std::string name;
	std::string graph_type; // "symbol_reference" | "call_graph"
	std::vector<GraphNode> nodes;
	std::vector<GraphEdge> edges;
};

const char *nodeTypeName(NodeType t);
const char *edgeTypeName(EdgeType t);

// ─── Relation Type Contract ────────────────────────────────────
// The SQLite `relation.type` column stores an integer that mirrors
// `EdgeType`. To keep call-graph semantics pure, only `EdgeType::Calls`
// (function/method invocations) is allowed in the LadybugDB `CALLS`
// table; every other typed relation goes to `RELATES` and retains its
// `edge_type` column for downstream disambiguation.
//
// This contract is the single source of truth for the SQLite ↔ LadybugDB
// mapping. Production code MUST NOT branch on raw integer thresholds
// such as `rtype >= 4`; it MUST call one of the helpers below.
//
// See `plan/rules/relation_contract.md` and Step 0 of
// `ACCURACY_IMPROVEMENT_DEVELOPMENT_PLAN.md` for the full rationale.

/// Convert an integer `relation.type` value to a strongly-typed EdgeType.
/// Out-of-range values map to `EdgeType::References` (a safe non-call
/// fallback) so unknown relation kinds never accidentally become CALLS.
EdgeType relationTypeFromInt(int rtype);

/// Convert an EdgeType to its integer storage form.
int relationTypeToInt(EdgeType type);

/// Whether a typed relation belongs in the LadybugDB `CALLS` table.
/// Only `EdgeType::Calls` (function/method invocations) returns true;
/// all other kinds (References, Defines, Contains, Imports, Inherits,
/// UsesType, HasType) return false and must be compiled to `RELATES`.
bool isCallsEdge(int rtype);
bool isCallsEdge(EdgeType type);

/// Whether a typed relation belongs in the LadybugDB `RELATES` table.
/// This is the logical complement of `isCallsEdge`. Non-call relations
/// retain their `edge_type` column so callers can still distinguish
/// References from Defines, Contains, Imports, etc.
bool isRelatesEdge(int rtype);
bool isRelatesEdge(EdgeType type);

} // namespace graph

#endif // GRAPH_TYPES_H
