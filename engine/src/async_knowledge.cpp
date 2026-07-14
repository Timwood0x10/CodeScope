#include "async_knowledge.h"
#include "engine_internal.h"
#include "store/store.h"
#include "model/engine.h"
#include "model/plugins/workflow.h"
#include "model/plugins/capability.h"
#include "model/plugins/architecture.h"
#include "model/plugins/contract.h"
#include "model/state_builder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <sqlite3.h>
#include <thread>

// ─── module_edge population ─────────────────────────────────────
//
// The module_edge table pre-computes cross-module call edges so that
// explain_module and project_overview can answer "which modules does
// module X depend on?" in O(modules) instead of O(entities). It is
// populated by a single SQL GROUP BY query over the relation table.
//
// The module identifier is the denormalized entity.module_path column
// (the directory portion of file_path, populated at entity INSERT time
// in store_graph.cpp). Using the indexed column instead of the
// rtrim(replace(...)) expression makes the GROUP BY sargable.

namespace
{

// Atomic flag ensuring only one async builder runs at a time.
std::atomic<bool> g_async_running{ false };

// Joinable thread handle — NOT detached, so engine_shutdown can join it
// before destroying g_store. Protected by g_thread_mutex to prevent
// data races between launch (writer) and join (reader in shutdown).
std::mutex g_thread_mutex;
std::thread g_builder_thread;

// Maximum number of module_edge rows to insert. Prevents unbounded
// growth on projects with many small modules (one per file).
inline constexpr int64_t kMaxModuleEdges = 5000;

} // namespace

int64_t buildKnowledgeGraphSync(store::GraphStore &store, uint64_t project_id)
{
	sqlite3 *db = store.handle();
	if (!db) {
		fprintf(stderr,
			"[module=async, method=buildKnowledgeGraphSync] "
			"db handle is null\n");
		return -1;
	}

	// Wrap DELETE + INSERT in a transaction so that either both succeed
	// or both are rolled back. Without this, a failure between the two
	// statements would leave the module_edge table empty.
	if (!store.exec("BEGIN IMMEDIATE")) {
		fprintf(stderr,
			"[module=async, method=buildKnowledgeGraphSync] "
			"BEGIN failed: %s\n",
			store.error().c_str());
		return -1;
	}

	std::string pid = std::to_string(project_id);
	std::string del_sql = "DELETE FROM module_edge WHERE project_id=" + pid;
	if (!store.exec(del_sql.c_str())) {
		fprintf(stderr,
			"[module=async, method=buildKnowledgeGraphSync] "
			"delete failed: %s\n",
			store.error().c_str());
		store.exec("ROLLBACK");
		return -1;
	}

	// Populate module_edge by grouping call edges (relation type=1) by
	// source module and target module. The module is the denormalized
	// entity.module_path column (directory of file_path). Same-file
	// edges are excluded.
	std::string insert_sql =
		"INSERT INTO module_edge "
		"(project_id, src_module, tgt_module, edge_count) "
		"SELECT " +
		pid +
		", "
		"src.module_path, "
		"tgt.module_path, "
		"COUNT(*) "
		"FROM relation r "
		"JOIN entity src ON r.source_id = src.id "
		"JOIN entity tgt ON r.target_id = tgt.id "
		"WHERE r.project_id=" +
		pid +
		" AND r.type=1 "
		"  AND src.file_path != tgt.file_path "
		"  AND src.file_path LIKE '%/%' "
		"  AND tgt.file_path LIKE '%/%' "
		"GROUP BY "
		"src.module_path, "
		"tgt.module_path "
		"LIMIT " +
		std::to_string(kMaxModuleEdges);
	if (!store.exec(insert_sql.c_str())) {
		fprintf(stderr,
			"[module=async, method=buildKnowledgeGraphSync] "
			"insert failed: %s\n",
			store.error().c_str());
		store.exec("ROLLBACK");
		return -1;
	}

	int64_t rows = sqlite3_changes(db);

	if (!store.exec("COMMIT")) {
		fprintf(stderr,
			"[module=async, method=buildKnowledgeGraphSync] "
			"COMMIT failed: %s\n",
			store.error().c_str());
		return -1;
	}

	// Set the knowledge_ready readiness flag so callers know the
	// knowledge graph is available for queries.
	store.setProjectReadiness(project_id, "knowledge_ready", 1);

	fprintf(stderr,
		"[module=async, method=buildKnowledgeGraphSync] "
		"populated %lld module_edge rows for project %s\n",
		(long long)rows, pid.c_str());
	return rows;
}

void runModelIndexSync(store::GraphStore &store, uint64_t project_id,
		       bool run_fts)
{
	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();

	// ── Model Engine: capability / contract / workflow / architecture ──
	// Supplementary layer — failures are logged but do NOT abort the
	// async path, since the core graph is already queryable.
	auto t_model_start = Clock::now();
	try {
		model::ModelEngine me(&store);
		me.addPlugin(std::make_unique<model::WorkflowPlugin>(&store));
		me.addPlugin(std::make_unique<model::CapabilityPlugin>(&store));
		me.addPlugin(
			std::make_unique<model::ArchitecturePlugin>(&store));
		me.addPlugin(std::make_unique<model::ContractPlugin>(&store));
		me.runAll(project_id);
	} catch (const std::exception &e) {
		fprintf(stderr,
			"[module=async, method=runModelIndexSync] "
			"ModelEngine failed: %s\n",
			e.what());
	} catch (...) {
		fprintf(stderr, "[module=async, method=runModelIndexSync] "
				"ModelEngine failed with unknown exception\n");
	}
	auto t_model = Clock::now();

	// ── State Builder: module-level summaries ──
	auto t_state_start = Clock::now();
	try {
		model::StateBuilder sb(&store, project_id);
		sb.buildAll();
	} catch (const std::exception &e) {
		fprintf(stderr,
			"[module=async, method=runModelIndexSync] "
			"StateBuilder failed: %s\n",
			e.what());
	} catch (...) {
		fprintf(stderr, "[module=async, method=runModelIndexSync] "
				"StateBuilder failed with unknown exception\n");
	}
	auto t_state = Clock::now();

	// ── FTS index (trigram + code_fts) ──
	// Skipped in fast mode — the caller passes run_fts=false when
	// CODESCOPE_INDEX_MODE=fast. FTS is the most expensive single step
	// here, so skipping it gives the fastest "index done" in fast mode.
	int64_t fts_ms = 0;
	if (run_fts) {
		auto t_fts_start = Clock::now();
		try {
			store.buildFTSFromGraph(project_id);
			store.setProjectReadiness(project_id, "fts_ready", 1);
		} catch (const std::exception &e) {
			fprintf(stderr,
				"[module=async, method=runModelIndexSync] "
				"FTS build failed: %s\n",
				e.what());
		} catch (...) {
			fprintf(stderr,
				"[module=async, method=runModelIndexSync] "
				"FTS build failed with unknown exception\n");
		}
		fts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				 Clock::now() - t_fts_start)
				 .count();
	}

	auto ms = [](auto start, auto end) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			       end - start)
			.count();
	};
	fprintf(stderr,
		"[module=async, method=runModelIndexSync] "
		"project %llu | model=%lldms state=%lldms fts=%lldms"
		" total=%lldms\n",
		(unsigned long long)project_id,
		(long long)ms(t_model_start, t_model),
		(long long)ms(t_state_start, t_state), (long long)fts_ms,
		(long long)ms(t0, Clock::now()));
}

void launchAsyncKnowledgeBuilder(uint64_t project_id, bool run_fts)
{
	std::lock_guard<std::mutex> lock(g_thread_mutex);

	// Join any previous builder thread before starting a new one.
	// This ensures we never have two builder threads running and
	// prevents thread handle leaks.
	if (g_builder_thread.joinable())
		g_builder_thread.join();

	// Only one builder runs at a time. If a build is already in progress,
	// skip — the caller's indexing will have triggered a new build, and
	// the running build will finish with stale data that the next build
	// will overwrite.
	bool expected = false;
	if (!g_async_running.compare_exchange_strong(expected, true)) {
		fprintf(stderr,
			"[module=async, method=launchAsyncKnowledgeBuilder] "
			"builder already running, skipping for project %llu\n",
			(unsigned long long)project_id);
		return;
	}

	g_builder_thread = std::thread([project_id, run_fts]() {
		if (!g_store) {
			fprintf(stderr,
				"[module=async] g_store is null, aborting\n");
			g_async_running.store(false);
			return;
		}
		fprintf(stderr,
			"[module=async] starting model+state+fts+knowledge"
			" build for project %llu (run_fts=%d)\n",
			(unsigned long long)project_id, (int)run_fts);
		try {
			// P3: Model Engine + State Builder + FTS run in the
			// background thread BEFORE the knowledge builder, so the
			// synchronous index path returns as soon as the core graph
			// + indexes are ready.
			runModelIndexSync(*g_store, project_id, run_fts);
			buildKnowledgeGraphSync(*g_store, project_id);
		} catch (const std::exception &e) {
			fprintf(stderr,
				"[module=async] build failed with exception: %s\n",
				e.what());
		} catch (...) {
			fprintf(stderr,
				"[module=async] build failed with unknown exception\n");
		}
		g_async_running.store(false);
		fprintf(stderr,
			"[module=async] knowledge graph build complete "
			"for project %llu\n",
			(unsigned long long)project_id);
	});
}

void joinAsyncKnowledgeBuilder()
{
	std::lock_guard<std::mutex> lock(g_thread_mutex);
	if (g_builder_thread.joinable())
		g_builder_thread.join();
}

bool isAsyncKnowledgeBuilderRunning()
{
	return g_async_running.load();
}
