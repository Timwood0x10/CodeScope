#ifndef GRAPH_TYPES_H
#define GRAPH_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace graph {

// ─── Node Types ────────────────────────────────────────────────

enum class NodeType : uint8_t {
  Function = 0,
  Method,
  Class,
  Struct,
  Interface,
  Variable,
  Module,
  File,
};

// ─── Edge Types ────────────────────────────────────────────────

enum class EdgeType : uint8_t {
  References = 0, // symbol reference
  Calls,          // function call
  Defines,        // defines a symbol
  Contains,       // class contains method, module contains function
  Imports,        // import/include
  Inherits,       // class inheritance (v2)
};

// ─── Graph Node ────────────────────────────────────────────────

struct GraphNode {
  uint64_t id = 0;
  uint64_t ir_node_id = 0; // back-reference to IR node
  NodeType type;
  std::string name;
  std::string qualified_name;
  std::string file_path;
  uint32_t start_row = 0, start_col = 0;
  uint32_t end_row = 0, end_col = 0;
  std::string language;
};

// ─── Graph Edge ────────────────────────────────────────────────

struct GraphEdge {
  uint64_t id = 0;
  uint64_t source_id = 0; // GraphNode.id
  uint64_t target_id = 0; // GraphNode.id
  EdgeType type;
  std::string graph_type; // "symbol_reference" | "call_graph"
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

} // namespace graph

#endif // GRAPH_TYPES_H
