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

/// Compile the SQLite graph for a project into LadybugDB.
///
/// Reads graph_nodes and graph_edges from SQLite, clears the project's
/// existing subgraph in LadybugDB (DETACH DELETE), then batch-inserts
/// all nodes and edges via batched Cypher UNWIND + CREATE statements
/// (100 per batch). Each node gets a deterministic UID based on
/// (project_id, file_path, graph_nodes.id).
///
/// @param store   The GraphStore with an open SQLite + LadybugDB connection.
/// @param project_id  The project whose graph should be compiled.
/// @param changed_files  When non-null and non-empty, only subgraph
///        nodes/edges that touch these files are recompiled (incremental
///        mode). When null or empty, the whole project graph is recompiled
///        (full mode, backward-compatible default).
/// @return true on success, false on failure (error logged to stderr).
bool compileGraphToLadybugDB(
	GraphStore *store, uint64_t project_id,
	const std::unordered_set<std::string> *changed_files = nullptr);

} // namespace store

#endif // CORESCOPE_STORE_GRAPH_COMPILER_H_