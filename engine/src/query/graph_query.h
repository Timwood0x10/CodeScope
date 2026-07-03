#ifndef GRAPH_QUERY_H
#define GRAPH_QUERY_H

#include "../store/store.h"
#include <cstdint>
#include <string>

namespace query {

// Parse a minimal graph-pattern DSL and execute against the store.
//
// Format: MATCH (srcType[:srcName])-[edgeType]->(tgtType[:tgtName])
//
// Node types use their string name (Function, Method, Class, etc.).
// Edge types use their string name (Calls, References, Contains, etc.).
// Empty type matches any (e.g. ()-[Calls]->(Function)).
// Optional :name filters by symbol name (e.g. (Function:add)).
//
// Returns JSON: { "total": N, "results": [ { source: ..., edge: ..., target:
// ... }, ... ] }
std::string executeGraphQuery(uint64_t project_id, const char *dsl_query,
                              store::GraphStore *store);

} // namespace query

#endif // GRAPH_QUERY_H
