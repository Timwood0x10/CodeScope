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
	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();

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

	// Populate the modules hierarchy table from entity.module_path.
	// Each distinct directory becomes one modules row; parent_id is
	// resolved by matching the next-shorter path prefix so that
	// getModuleTreeJson can render a nested tree. file_count is the
	// number of entity rows in that directory; language is the majority
	// language among those rows.
	populateModulesHierarchy(store, project_id);

	// Set the knowledge_ready readiness flag so callers know the
	// knowledge graph is available for queries.
	store.setProjectReadiness(project_id, "knowledge_ready", 1);

	fprintf(stderr,
		"[module=async, method=buildKnowledgeGraphSync] "
		"populated %lld module_edge rows for project %s"
		" (%lldms)\n",
		(long long)rows, pid.c_str(),
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t0)
			.count());
	return rows;
}

// Populate the modules hierarchy table from entity.module_path.
//
// entity.module_path holds the absolute directory portion of each file
// (populated at entity INSERT time in store_graph.cpp). This helper
// collapses those directories into one modules row per distinct path,
// with parent_id resolved by the next-shorter prefix so that
// getModuleTreeJson can render a nested tree without further joins.
//
// Algorithm:
//   1. Resolve the project root_path from the projects table and trim
//      it from every module_path so modules.path stays project-relative
//      (matches the schema comment in store_schema.cpp §"modules").
//   2. Walk the distinct directories in path-length order so every
//      parent is inserted before its children; insertModule's existence
//      check then resolves parent_id by looking up the prefix path.
//   3. file_count = entity rows in that exact directory; language =
//      majority language among those rows (empty if no entity rows).
//
// All failures surface a stderr line tagged with module=async,
// method=populateModulesHierarchy per the error-trace policy in
// plan/rules/code_rules.md §"Additional Rules". Returns the number of
// modules rows inserted; -1 on a hard failure.
int64_t populateModulesHierarchy(store::GraphStore &store, uint64_t project_id)
{
	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();

	sqlite3 *db = store.handle();
	if (!db) {
		fprintf(stderr,
			"[module=async, method=populateModulesHierarchy] "
			"db handle is null\n");
		return -1;
	}

	// Resolve project root_path so modules.path can be stored
	// project-relative (schema contract). projects.root_path is the
	// canonical project root recorded at engine_create_project time.
	std::string root_path;
	{
		sqlite3_stmt *rp = nullptr;
		if (sqlite3_prepare_v2(
			    db, "SELECT root_path FROM projects WHERE id=?", -1,
			    &rp, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"[module=async, method=populateModulesHierarchy] "
				"prepare root_path failed: %s\n",
				sqlite3_errmsg(db));
			return -1;
		}
		sqlite3_bind_int64(rp, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(rp) == SQLITE_ROW) {
			const char *p = reinterpret_cast<const char *>(
				sqlite3_column_text(rp, 0));
			if (p)
				root_path = p;
		}
		sqlite3_finalize(rp);
	}
	// Normalise: ensure root ends with '/' so prefix trimming is a
	// simple `root_path` removal without leaving a leading slash.
	if (!root_path.empty() && root_path.back() != '/')
		root_path.push_back('/');

	// Pull every distinct module_path with its entity count and the
	// majority language in one pass. module_path is already the
	// directory portion, so one row per directory.
	sqlite3_stmt *q = nullptr;
	const char *qsql =
		"SELECT module_path, COUNT(*) AS cnt, "
		"  (SELECT language FROM entity e2 "
		"   WHERE e2.project_id = ? AND e2.module_path = e.module_path "
		"   GROUP BY language ORDER BY COUNT(*) DESC LIMIT 1) AS lang "
		"FROM entity e "
		"WHERE project_id = ? AND module_path != '' "
		"GROUP BY module_path "
		"ORDER BY module_path";
	if (sqlite3_prepare_v2(db, qsql, -1, &q, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=async, method=populateModulesHierarchy] "
			"prepare modules failed: %s\n",
			sqlite3_errmsg(db));
		return -1;
	}
	sqlite3_bind_int64(q, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(q, 2, static_cast<int64_t>(project_id));

	// Materialise into a vector: we need two passes (parent insert
	// requires the prefix row id, which is only known after insert).
	// Ordering by module_path ASC guarantees parent directories
	// (shorter prefixes) appear before their children, so insertModule
	// can resolve parent_id by looking up the trimmed prefix path.
	struct ModRow {
		std::string rel_path; // project-relative
		std::string name; // last path segment
		std::string language;
		int file_count;
	};
	std::vector<ModRow> rows;
	while (sqlite3_step(q) == SQLITE_ROW) {
		const char *mp = reinterpret_cast<const char *>(
			sqlite3_column_text(q, 0));
		int cnt = sqlite3_column_int(q, 1);
		const char *lg = reinterpret_cast<const char *>(
			sqlite3_column_text(q, 2));
		if (!mp || !*mp)
			continue;
		std::string abs = mp;
		// Strip project root to get the project-relative path stored
		// in modules.path (schema contract).
		std::string rel;
		if (!root_path.empty() && abs.rfind(root_path, 0) == 0)
			rel = abs.substr(root_path.size());
		else
			rel = abs;
		if (!rel.empty() && rel.back() == '/')
			rel.pop_back();
		if (rel.empty())
			continue;
		// Module name = last non-empty path segment.
		auto slash = rel.rfind('/');
		std::string name = (slash == std::string::npos) ?
					   rel :
					   rel.substr(slash + 1);
		rows.push_back({ rel, name, lg ? lg : "", cnt });
	}
	sqlite3_finalize(q);

	// Insert each directory in path-length order so parents precede
	// children. insertModule is idempotent (it checks path uniqueness
	// first), so re-runs after partial indexing don't duplicate rows.
	// parent_id is resolved by trimming the last path segment and
	// looking up the resulting prefix via insertModule (which returns
	// the existing row id without inserting when the path already
	// exists).
	int64_t inserted = 0;
	for (const auto &r : rows) {
		// Resolve parent: drop the trailing segment, look up its id.
		uint64_t parent_id = 0;
		auto slash = r.rel_path.rfind('/');
		if (slash != std::string::npos) {
			std::string parent_path = r.rel_path.substr(0, slash);
			// insertModule returns the existing id when the path is
			// already present, so this is a pure lookup for the
			// parent (never inserts a stub).
			parent_id = store.insertModule(project_id, 0,
						       parent_path.c_str(),
						       parent_path.c_str(), "");
			// If the parent path wasn't already inserted (shouldn't
			// happen given ASC ordering, but guard anyway), leave
			// parent_id = 0 so the row becomes a root.
			if (parent_id == 0 && !parent_path.empty()) {
				// Insert the missing parent as a placeholder so
				// the child has a real parent_id. Language is
				// empty; file_count stays 0 until entity rows
				// appear under it.
				parent_id = store.insertModule(
					project_id, 0, parent_path.c_str(),
					parent_path.c_str(), "");
			}
		}
		uint64_t mid = store.insertModule(project_id, parent_id,
						  r.name.c_str(),
						  r.rel_path.c_str(),
						  r.language.c_str());
		if (mid == 0) {
			fprintf(stderr,
				"[module=async, method=populateModulesHierarchy] "
				"insertModule failed for path '%s': %s\n",
				r.rel_path.c_str(), store.error().c_str());
			continue;
		}
		// Update file_count for this module (insertModule does not
		// set it; default 0). One UPDATE per row is acceptable for
		// ~250 modules; for larger projects this could be batched.
		// language value originates from entity.language (a fixed
		// enum: c/cpp/python/rust/...), so single-quote wrapping is
		// injection-safe here — no user-controlled characters.
		std::string upd = "UPDATE modules SET file_count=" +
				  std::to_string(r.file_count) +
				  ", language='" + r.language +
				  "' WHERE id=" + std::to_string(mid);
		if (!store.exec(upd.c_str())) {
			fprintf(stderr,
				"[module=async, method=populateModulesHierarchy] "
				"UPDATE file_count failed for id %llu: %s\n",
				(unsigned long long)mid, store.error().c_str());
		}
		inserted++;
	}

	fprintf(stderr,
		"[module=async, method=populateModulesHierarchy] "
		"inserted %lld modules rows for project %llu (%lldms)\n",
		(long long)inserted, (unsigned long long)project_id,
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t0)
			.count());
	return inserted;
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
