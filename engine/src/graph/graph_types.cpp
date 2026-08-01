#include "graph_types.h"

namespace graph
{

const char *nodeTypeName(NodeType t)
{
	switch (t) {
	case NodeType::Function:
		return "function";
	case NodeType::Method:
		return "method";
	case NodeType::Class:
		return "class";
	case NodeType::Struct:
		return "struct";
	case NodeType::Interface:
		return "interface";
	case NodeType::Variable:
		return "variable";
	case NodeType::Macro:
		return "macro";
	case NodeType::Module:
		return "module";
	case NodeType::File:
		return "file";
	}
	return "unknown";
}

const char *edgeTypeName(EdgeType t)
{
	switch (t) {
	case EdgeType::References:
		return "references";
	case EdgeType::Calls:
		return "calls";
	case EdgeType::Defines:
		return "defines";
	case EdgeType::Contains:
		return "contains";
	case EdgeType::Imports:
		return "imports";
	case EdgeType::Inherits:
		return "inherits";
	case EdgeType::UsesType:
		return "uses_type";
	case EdgeType::HasType:
		return "has_type";
	}
	return "unknown";
}

// ─── Relation Type Contract Implementation ─────────────────────
// See graph_types.h for the full contract. These helpers are the only
// sanctioned way to map between SQLite `relation.type` integers and
// the LadybugDB CALLS / RELATES tables.

EdgeType relationTypeFromInt(int rtype)
{
	switch (rtype) {
	case static_cast<int>(EdgeType::References):
		return EdgeType::References;
	case static_cast<int>(EdgeType::Calls):
		return EdgeType::Calls;
	case static_cast<int>(EdgeType::Defines):
		return EdgeType::Defines;
	case static_cast<int>(EdgeType::Contains):
		return EdgeType::Contains;
	case static_cast<int>(EdgeType::Imports):
		return EdgeType::Imports;
	case static_cast<int>(EdgeType::Inherits):
		return EdgeType::Inherits;
	case static_cast<int>(EdgeType::UsesType):
		return EdgeType::UsesType;
	case static_cast<int>(EdgeType::HasType):
		return EdgeType::HasType;
	default:
		// Unknown relation kinds are treated as References — a
		// non-call fallback so they can never pollute CALLS.
		return EdgeType::References;
	}
}

int relationTypeToInt(EdgeType type)
{
	return static_cast<int>(type);
}

bool isCallsEdge(int rtype)
{
	return relationTypeFromInt(rtype) == EdgeType::Calls;
}

bool isCallsEdge(EdgeType type)
{
	return type == EdgeType::Calls;
}

bool isRelatesEdge(int rtype)
{
	return !isCallsEdge(rtype);
}

bool isRelatesEdge(EdgeType type)
{
	return !isCallsEdge(type);
}

} // namespace graph
