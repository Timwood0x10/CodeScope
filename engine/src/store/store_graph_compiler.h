// store_graph_compiler.h
//
// Graph Compiler: compiles SQLite graph data into LadybugDB.
//
// Per the db_res.md design, LadybugDB is the Graph Engine, not a cache
// or sync target. The Graph Compiler reads from SQLite graph_nodes and
// graph_edges (which are built by buildGraph from semantic_records) and
// writes them into LadybugDB as GraphNode nodes + CALLS/RELATES edges.
//
// This is a one-way compile: SQLite → LadybugDB. LadybugDB is never
// read back into SQLite. If the compile fails, the SQLite graph remains
// the source of truth and hasLadybugDB() returns false.

#ifndef CORESCOPE_STORE_GRAPH_COMPILER_H_
#define CORESCOPE_STORE_GRAPH_COMPILER_H_

#include <cstdint>
#include <string>
#include <unordered_set>

namespace store
{

class GraphStore;

/// Build LadybugDB graph from entity/relation tables.
///
/// Reads entity and relation tables from SQLite, clears the project's
/// existing subgraph in LadybugDB (DETACH DELETE), then bulk-inserts
/// all nodes and edges via CSV + Kuzu COPY FROM. Uses the same
/// GraphNode label and CALLS/RELATES edge tables as the old
/// compileGraphToLadybugDB, so query tools that already query
/// LadybugDB work without changes.
///
/// This is the replacement for compileGraphToLadybugDB. The old
/// function reads from graph_nodes/graph_edges; this one reads from
/// entity/relation, which are the canonical source tables.
///
/// @param store   The GraphStore with an open SQLite + LadybugDB connection.
/// @param project_id  The project whose graph should be compiled.
/// @param changed_files  When non-null and non-empty, only subgraph
///        nodes/edges that touch these files are recompiled (incremental
///        mode). When null or empty, the whole project graph is
///        recompiled (full mode).
/// @return true on success, false on failure (error logged to stderr).
bool buildLadybugFromEntityRelation(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files = nullptr);

/// Parallel multi-project LadybugDB rebuild: exports every project's
/// CSV concurrently (read-only SQLite connections) then COPY FROMs each
/// project serially on the single Kuzu connection. Produces the same
/// graph as calling buildLadybugFromEntityRelation per project.
bool buildLadybugGraphsParallel(GraphStore *store,
				const std::vector<uint64_t> &project_ids);

/// DEPRECATED: Use buildLadybugFromEntityRelation instead.
/// Reads graph_nodes/graph_edges tables (old schema) and compiles
/// into LadybugDB. Kept for backward compatibility during migration.
bool compileGraphToLadybugDB(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files = nullptr);

} // namespace store

#endif // CORESCOPE_STORE_GRAPH_COMPILER_H_