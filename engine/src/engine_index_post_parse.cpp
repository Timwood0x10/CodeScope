// Shared post-parse sequence for engine_index_project.
//
// Both the streaming path (BoundedQueue + single writer) and the in-memory
// bulk path (store::MemBulkAggregator) produce semantic_records identically.
// After the FileResult data is persisted, this single function runs the
// graph-building pipeline, so the two paths cannot drift. The data-processing
// logic here is unchanged from the previous inline block in
// engine_index_project.cpp.

#include "engine_internal.h"
#include "async_knowledge.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#include <sqlite3.h>

using namespace std::chrono;

char *engine_index_post_parse(uint64_t project_id, const std::string &dir,
			      const std::vector<std::string> &job_paths,
			      const FilterPolicy &filter, bool is_reindex,
			      bool mode_fast, bool mode_deep,
			      int64_t time_parse_ms, int64_t time_buildgraph_ms,
			      int total_indexed)
{
	// ── Post-loop: GraphFinalize ──────────────────────────────
	// After all data is written, build the graph, populate symbols,
	// copy cross-file call edges, build FTS, and resolve metrics.
	// This replaces the old enhance phase — no re-parse needed.

	// Update progress
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = (int)job_paths.size();
		p.current_file = total_indexed;
		p.phase = 3; // "building_graph"
		p.percent = 85;
		store::setIndexProgress(p);
	}

	// ── Step 1: buildGraph (SQL-only, graph_nodes + graph_edges + CSR) ──
	// Reads semantic_records, creates graph_nodes/graph_edges via SQL JOINs.
	// NOTE: calls=true builds call edges via all priorities in
	// buildCallEdgesSQL (store_intern.cpp): P1 (intra-file ref_original_id),
	// P2 (translator-resolved), P3 (name-based cross-file, language-filtered
	// and capped per-caller-per-name to avoid cartesian explosion), and
	// P3b (short-name fallback). Memory: O(SQLite cache_size), not O(nodes).
	int64_t time_fts_ms = 0, time_vector_ms = 0;
	{
		auto t_bg = steady_clock::now();
		g_store->beginTransaction();
		// Incremental re-index: pass changed files so buildGraph uses the
		// optimized path (only cycles the unique edge index, not the 5
		// lookup indexes). First index (is_reindex=false) passes nullptr
		// for a full rebuild.
		std::unordered_set<std::string> changed_files;
		if (is_reindex) {
			changed_files.reserve(job_paths.size());
			for (const auto &path : job_paths)
				changed_files.insert(path);
		}
		g_store->buildGraph(project_id, true,
				    is_reindex ? &changed_files : nullptr);
		g_store->commitTransaction();
		// Indexing now builds the full call graph (buildGraph above),
		// so mark every node callgraph_ready. This makes trace_path and
		// the enhancement-status report reflect that the call graph is
		// present immediately after index — even before any (now no-op)
		// enhance pass. Enhance increments the flag idempotently, so
		// reruns leave callgraph_ready unchanged.
		{
			std::string up =
				"UPDATE graph_nodes SET callgraph_ready=1 "
				"WHERE project_id=" +
				std::to_string(project_id);
			// This UPDATE runs AFTER commitTransaction(), so a
			// failure here is NOT rolled back and leaves callgraph_ready=0
			// on every node while the call graph itself IS committed.
			// Downstream trace_path / enhancement-status would then
			// silently misreport readiness. Log on failure so the silent
			// misreport is at least observable, mirroring the
			// [module=..., method=...] fprintf pattern used elsewhere.
			if (!g_store->exec(up.c_str())) {
				fprintf(stderr,
					"engine_index_project: callgraph_ready UPDATE "
					"failed: %s "
					"[module=engine, method=engine_index_project]\n",
					g_store->error().c_str());
			}
		}
		// P0.1: entity/relation dual-write now happens INSIDE buildGraph
		// (store_graph.cpp). The previous duplicate INSERT...SELECT here
		// ran the same work twice, doubling wall time for large projects.
		time_buildgraph_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_bg)
				.count();
	}

	// P3: FTS index construction moved to the async path
	// (runModelIndexSync in async_knowledge.cpp). This keeps the
	// synchronous index path fast — the user sees "normal_ready" as
	// soon as the core graph + indexes are built, while FTS materialises
	// in the background. time_fts_ms stays 0 here (reported by async log).

	// ── Step 5: Resolve staged metrics → metrics + symbol_status ──
	// Pre-computed metrics (from parse workers) are resolved via
	// (file_path, name, line) JOIN with symbols.
	{
		auto t_metrics = steady_clock::now();
		g_store->resolveStagedMetrics(project_id);
		fprintf(stderr,
			"engine: resolveStagedMetrics=%lldms "
			"[module=engine, method=engine_index_project]\n",
			(long long)duration_cast<milliseconds>(
				steady_clock::now() - t_metrics)
				.count());
	}

	// DEEP mode: build vectors (NORMAL skips them)
	if (mode_deep) {
		auto t_v = steady_clock::now();
		g_store->buildVectorsFromGraph(project_id);
		g_store->setProjectReadiness(project_id, "vector_ready", 1);
		time_vector_ms =
			duration_cast<milliseconds>(steady_clock::now() - t_v)
				.count();
	}

	// Build deferred indexes after bulk load. BulkPragmaGuard tunes
	// PRAGMA synchronous=OFF + cache_size=64 MB for maximum index-creation
	// throughput (matches codebase-memory-mcp cbm_store_begin_bulk/end_bulk),
	// then restores previous values. full_rebuild=!is_reindex: skip lookup
	// index recreation on incremental runs (they were never dropped); always
	// run dedup DELETE + unique edge index creation.
	{
		store::GraphStore::BulkPragmaGuard guard(g_store.get());
		auto t_idx = steady_clock::now();
		g_store->createIndexesAfterBulkLoad(project_id, !is_reindex);
		fprintf(stderr,
			"engine: createIndexesAfterBulkLoad=%lldms "
			"[module=engine, method=engine_index_project]\n",
			(long long)duration_cast<milliseconds>(
				steady_clock::now() - t_idx)
				.count());
	}

	// Set readiness flag — core graph is queryable now.
	// fts_ready / knowledge_ready are set by the async path
	// (runModelIndexSync + buildKnowledgeGraphSync).
	g_store->setProjectReadiness(project_id, "normal_ready", 1);

	// P0.2 / P3: Model Engine + State Builder + FTS moved to async path.
	// Previously ran synchronously here AND inside buildGraph (double
	// work). Now runs once, in the background thread launched below via
	// launchAsyncKnowledgeBuilder(project_id, !mode_fast).

	// ── Step 7: Async Knowledge Graph Construction ──
	// Launch a background thread to populate the module_edge table
	// (cross-module dependency edges). Non-blocking — the index result
	// is returned immediately while the knowledge graph materialises
	// concurrently. Callers can poll isAsyncKnowledgeBuilderRunning()
	// or check the "knowledge_ready" readiness flag.
	// NOTE: This is launched AFTER all g_store reads below, to avoid
	// concurrent GraphStore access. The builder writes to module_edge
	// while the main thread reads graph_nodes/graph_edges counts.

	// ── Result JSON ──────────────────────────────────────────────
	std::ostringstream result;
	result << "{\"ok\":true,\"files_indexed\":" << total_indexed
	       << ",\"workers\":"
	       << std::min(static_cast<int>(job_paths.size()),
			   static_cast<int>(
				   std::thread::hardware_concurrency()));
	if (time_parse_ms > 0)
		result << ",\"time_parse_ms\":" << time_parse_ms
		       << ",\"time_sqlite_ms\":0"
		       << ",\"time_buildgraph_ms\":" << time_buildgraph_ms;
	result << ",\"time_fts_ms\":" << time_fts_ms
	       << ",\"time_vector_ms\":" << time_vector_ms;

	// Add counts from graph tables
	{
		sqlite3_stmt *stmt = nullptr;
		std::string sql;
		sql = "SELECT COUNT(*) FROM graph_nodes WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_nodes\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		sql = "SELECT COUNT(*) FROM graph_edges WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_edges\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		// Also report symbols and call_edges counts
		sql = "SELECT COUNT(*) FROM graph_nodes WHERE project_id = " +
		      std::to_string(project_id);
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_symbols\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
		sql = "SELECT COUNT(*) FROM graph_edges WHERE project_id = " +
		      std::to_string(project_id) + " AND edge_type = 1";
		if (sqlite3_prepare_v2(g_store->handle(), sql.c_str(), -1,
				       &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				result << ",\"total_call_edges\":"
				       << sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		}
	}
	// Discovery stats
	auto &fs = filter.stats();
	result << ",\"discovery\":{\"seen_dirs\":" << fs.seen_dirs
	       << ",\"seen_files\":" << fs.seen_files
	       << ",\"skipped_dirs\":" << fs.skipped_dirs
	       << ",\"skipped_files\":" << fs.skipped_files
	       << ",\"skipped_suffix\":" << fs.skipped_suffix
	       << ",\"candidate_files\":" << fs.candidate_files << "}";
	result << "}";
	// Mark progress as done
	{
		store::IndexProgress p;
		p.project_id = project_id;
		p.total_files = (int)job_paths.size();
		p.current_file = (int)job_paths.size();
		p.phase = 5;
		p.percent = 100;
		store::setIndexProgress(p);
	}

	// Launch the async knowledge builder AFTER all g_store reads/writes
	// above are complete, to avoid concurrent GraphStore access.
	// run_fts=!mode_fast: fast mode skips FTS entirely; normal/deep
	// modes build FTS in the background thread.
	launchAsyncKnowledgeBuilder(project_id, !mode_fast);

	return dupString(result.str());
}
