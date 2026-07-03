#include "graph_types.h"

namespace graph {

const char* nodeTypeName(NodeType t) {
    switch (t) {
        case NodeType::Function:  return "function";
        case NodeType::Method:    return "method";
        case NodeType::Class:     return "class";
        case NodeType::Struct:    return "struct";
        case NodeType::Interface: return "interface";
        case NodeType::Variable:  return "variable";
        case NodeType::Module:    return "module";
        case NodeType::File:      return "file";
    }
    return "unknown";
}

const char* edgeTypeName(EdgeType t) {
    switch (t) {
        case EdgeType::References: return "references";
        case EdgeType::Calls:      return "calls";
        case EdgeType::Defines:    return "defines";
        case EdgeType::Contains:   return "contains";
        case EdgeType::Imports:    return "imports";
        case EdgeType::Inherits:   return "inherits";
    }
    return "unknown";
}

} // namespace graph
