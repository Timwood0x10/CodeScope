#ifndef GRAPH_QUERY_H
#define GRAPH_QUERY_H

#include <cstdint>
#include <string>
#include "../store/store.h"

namespace query
{

// Parse a minimal graph-pattern DSL and execute against the store.
//
// Format: MATCH (srcType[:srcName])-[edgeType]->(tgtType[:tgtName])
//
// Node types use their string name (Function, Method, Class, etc.).
// Edge types use their string name (Calls, References, Contains, etc.).
// Empty type matches any (e.g. ()-[Calls]->(Function)).
// Optional :name filters by symbol name (e.g. (Function:add)).
// Optional trailing `LIMIT <n>` (case-insensitive) caps the result set:
//   MATCH (Function)-[Calls]->(Function) LIMIT 100
// The clause is honoured, not decoration: without it a broad pattern is
// bounded only by the internal scan cap, which for a large project can exceed
// the MCP transport's per-message limit. Any OTHER trailing text is rejected
// with an error instead of being ignored.
//
// Returns JSON: { "total": N, "results": [ { source: ..., edge: ..., target:
// ... }, ... ] } — plus "truncated": true when a bound (LIMIT, the multi-hop
// row cap or the expansion budget) cut the result set short.
std::string executeGraphQuery(uint64_t project_id, const char *dsl_query,
			      store::GraphStore *store);

} // namespace query

#endif // GRAPH_QUERY_H
