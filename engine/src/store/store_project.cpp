#include "util/json_writer.h"
#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

#include "../graph/graph_builder.h"
#include "../ir/semantic_unit.h"

namespace store
{

uint64_t GraphStore::insertModule(uint64_t project_id, uint64_t parent_id,
				  const char *name, const char *path,
				  const char *language)
{
	// Check if module already exists at this path
	const char *check_sql =
		"SELECT id FROM modules WHERE project_id = ? AND path = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			uint64_t id = static_cast<uint64_t>(
				sqlite3_column_int64(stmt, 0));
			sqlite3_finalize(stmt);
			return id;
		}
		sqlite3_finalize(stmt);
	}

	const char *sql =
		"INSERT INTO modules (project_id, parent_id, name, path, language) "
		"VALUES (?, ?, ?, ?, ?)";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertModule: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (parent_id > 0)
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(parent_id));
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, language, -1, SQLITE_TRANSIENT);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertModule: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}
// ── New Schema (Phase A): Queries ─────────────────────────────

std::string GraphStore::getModuleTreeJson(uint64_t project_id)
{
	// Fetch all modules for the project
	const char *sql =
		"SELECT id, parent_id, name, path, language, file_count "
		"FROM modules WHERE project_id = ? ORDER BY path";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		return "{\"error\":\"getModuleTreeJson: prepare failed\"}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	// Build flat list first
	struct ModuleInfo {
		uint64_t id;
		uint64_t parent_id;
		std::string name;
		std::string path;
		std::string language;
		int file_count;
	};
	std::vector<ModuleInfo> modules;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ModuleInfo m;
		m.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		m.parent_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ?
				      0 :
				      static_cast<uint64_t>(
					      sqlite3_column_int64(stmt, 1));
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		m.name = n ? n : "";
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		m.path = p ? p : "";
		const char *l = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		m.language = l ? l : "";
		m.file_count = sqlite3_column_int(stmt, 5);
		modules.push_back(std::move(m));
	}
	sqlite3_finalize(stmt);

	if (modules.empty()) {
		return "{\"modules\":[]}";
	}

	// Build tree: find roots (parent_id == 0), then output nested children.
	//
	// IMPORTANT: the "first" flag controlling the leading-comma must be
	// scoped PER array, not shared across the whole recursion. A shared
	// flag would stay false after the first root and cause every children
	// array to emit a leading comma (",{...},{...}") — invalid JSON that
	// breaks json.loads on the client. We thread it as a parameter so
	// each sibling list owns its own flag.
	std::ostringstream json;
	json << "{\"modules\":[";
	// Build child map: parent_id → list of child module ids
	std::unordered_map<uint64_t, std::vector<uint64_t>> children_of;
	for (const auto &m : modules)
		children_of[m.parent_id].push_back(m.id);
	std::function<void(uint64_t, int, bool &)> outMod =
		[&](uint64_t id, int depth, bool &first) {
			auto it = std::find_if(modules.begin(), modules.end(),
					       [id](const ModuleInfo &m) {
						       return m.id == id;
					       });
			if (it == modules.end())
				return;
			if (!first)
				json << ",";
			first = false;
			// The element stays open across the optional children array
			// below (and the recursion writes nested elements into the
			// same stream), so its shape is kept; the string values go
			// through the shared escaper.
			json << "{\"id\":" << it->id
			     << ",\"parent_id\":" << it->parent_id
			     << ",\"depth\":" << depth << ",\"name\":\""
			     << util::jsonEscapeString(it->name) << "\""
			     << ",\"path\":\""
			     << util::jsonEscapeString(it->path) << "\""
			     << ",\"language\":\""
			     << util::jsonEscapeString(it->language) << "\""
			     << ",\"file_count\":" << it->file_count;
			auto ci = children_of.find(id);
			if (ci != children_of.end() && !ci->second.empty()) {
				json << ",\"children\":[";
				bool cf = true;
				for (auto cid : ci->second)
					outMod(cid, depth + 1, cf);
				json << "]";
			}
			json << "}";
		};
	bool root_first = true;
	for (const auto &m : modules)
		if (m.parent_id == 0)
			outMod(m.id, 0, root_first);
	json << "]}";
	return json.str();
}

std::string GraphStore::findSymbolJson(uint64_t project_id, const char *name)
{
	bool has_separator =
		(strstr(name, "::") != nullptr || strstr(name, ".") != nullptr);

	const char *sql;
	if (has_separator) {
		sql = "SELECT e.id, e.kind, e.name, "
		      "COALESCE(e.qualified_name, e.name), "
		      "e.file_path, e.language, e.start_row AS line, e.start_col AS column "
		      "FROM entity e WHERE e.project_id = ? AND e.qualified_name = ?";
	} else {
		sql = "SELECT e.id, e.kind, e.name, "
		      "COALESCE(e.qualified_name, e.name), "
		      "e.file_path, e.language, e.start_row AS line, e.start_col AS column "
		      "FROM entity e WHERE e.project_id = ? AND e.name = ?";
	}
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return "{\"error\":\"findSymbolJson: prepare failed\",\"results\":[]}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

	std::ostringstream json;
	json << "{\"results\":[";
	bool first = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (!first)
			json << ",";
		first = false;
		uint64_t id =
			static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
		const char *kind = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *sym_name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *sig = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		const char *lang = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		int line = sqlite3_column_int(stmt, 6);
		int col = sqlite3_column_int(stmt, 7);

		util::JsonWriter el;
		el.beginObject();
		el.key("id").value(id);
		// kind and language went through the writer now too: they are
		// free text read from the database and were previously emitted
		// unescaped.
		el.key("kind").value(kind ? kind : "");
		el.key("name").value(sym_name ? sym_name : "");
		el.key("signature").value(sig ? sig : "");
		el.key("language").value(lang ? lang : "");
		el.key("file_path").value(fp ? fp : "");
		el.key("line").value(line);
		el.key("column").value(col);
		el.endObject();
		json << el.str();
	}
	sqlite3_reset(stmt);
	json << "]}";

	// If the entity table returned nothing, fall back to graph_nodes. That
	// table is written only by the LEGACY engine_index_batch path, so this
	// fallback serves DBs built by it; the canonical pipeline (and the
	// primary query above) reads `entity`. Kept deliberately: a batch-built
	// DB must still resolve symbols.
	if (first) {
		std::ostringstream gn_json;
		gn_json << "{\"results\":[";
		bool gn_first = true;
		const char *gn_sql =
			"SELECT id, node_type, name, file_path, start_row, start_col, "
			"end_row, end_col, language "
			"FROM graph_nodes WHERE project_id = ? AND name = ? "
			"AND node_type IN (0,1,2,3,4,6) LIMIT 20";
		sqlite3_stmt *gn_stmt = nullptr;
		if (sqlite3_prepare_v2(db_, gn_sql, -1, &gn_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(gn_stmt, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_text(gn_stmt, 2, name, -1,
					  SQLITE_TRANSIENT);
			static const char *type_names[] = {
				"function",  "method", "class",
				"interface", "enum",   "typealias",
				"variable",
			};
			while (sqlite3_step(gn_stmt) == SQLITE_ROW) {
				if (!gn_first)
					gn_json << ",";
				gn_first = false;
				uint64_t id = static_cast<uint64_t>(
					sqlite3_column_int64(gn_stmt, 0));
				int nt = sqlite3_column_int(gn_stmt, 1);
				const char *n = reinterpret_cast<const char *>(
					sqlite3_column_text(gn_stmt, 2));
				const char *fp = reinterpret_cast<const char *>(
					sqlite3_column_text(gn_stmt, 3));
				int sr = sqlite3_column_int(gn_stmt, 4);
				int sc = sqlite3_column_int(gn_stmt, 5);
				const char *lang =
					reinterpret_cast<const char *>(
						sqlite3_column_text(gn_stmt,
								    8));
				const char *type_name = (nt >= 0 && nt < 7) ?
								type_names[nt] :
								"symbol";
				util::JsonWriter el;
				el.beginObject();
				el.key("id").value(id);
				// kind and language are free text and were emitted
				// unescaped before; the writer escapes them now.
				el.key("kind").value(type_name ? type_name :
								 "");
				el.key("name").value(n ? n : "");
				el.key("file_path").value(fp ? fp : "");
				el.key("line").value(sr);
				el.key("column").value(sc);
				el.key("language").value(lang ? lang : "");
				el.endObject();
				gn_json << el.str();
			}
			sqlite3_finalize(gn_stmt);
		}
		gn_json << "]}";
		return gn_json.str();
	}

	return json.str();
}
// ── Phase C: Unified Queries ──────────────────────────────────

namespace
{
// ── Canonical readiness probes ──────────────────────────────────
// The coverage ratios below are the same questions engine_queries_status.cpp
// answers for engine_get_enhancement_status, and they deliberately use the
// same SQL so both APIs report identical numbers. They read the canonical
// tables the pipeline actually writes (`entity`, `relation`, `node_vectors`).
//
// Step 10.1: they used to read `graph_nodes.<field>_ready`, a table the
// canonical pipeline never populates (only the legacy engine_index_batch path
// does), so every ratio was 0.0 — and consumers drew the wrong conclusion from
// it: `project_overview` reported a working call graph as
// `"callgraph_available": false`, `ready_features.call_graph: false` and an
// empty `sample_call_edges`, while find_callers/find_callees answered with
// hundreds of callers. verifier.h states the rule this violated: the canonical
// fact layer is the production source of truth, never graph_nodes.

/// Population the per-entity ratios are computed over: function and method
/// entities.
constexpr const char *kReadyEligibleSql =
	"SELECT COUNT(*) FROM entity WHERE project_id=? AND kind IN (0,1)";

/// Function/method entities participating in the call graph (either end of a
/// Calls relation).
constexpr const char *kReadyCallgraphSql =
	"SELECT COUNT(*) FROM (SELECT DISTINCT node FROM ("
	"SELECT source_id AS node FROM relation WHERE project_id=? AND type=1 "
	"UNION "
	"SELECT target_id AS node FROM relation WHERE project_id=? AND type=1))";

/// Function/method entities whose metrics were resolved (cyclomatic > 0).
constexpr const char *kReadyMetricsSql =
	"SELECT COUNT(*) FROM entity WHERE project_id=? AND kind IN (0,1) "
	"AND cyclomatic > 0";

/// Entities carrying a semantic vector: the canonical embedding coverage.
constexpr const char *kReadyEmbeddingSql =
	"SELECT COUNT(*) FROM node_vectors WHERE project_id=?";

/// Run a count probe. Returns -1 when the statement cannot be run, so the
/// caller can report the failure instead of silently returning 0.0.
/// \param db      Open store handle.
/// \param sql     Probe with one placeholder per entry of `binds`.
/// \param binds   Values bound to the placeholders, in order.
int64_t readyCount(sqlite3 *db, const char *sql,
		   const std::vector<int64_t> &binds)
{
	if (!db || !sql)
		return -1;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return -1;
	for (size_t i = 0; i < binds.size(); ++i)
		sqlite3_bind_int64(stmt, static_cast<int>(i + 1), binds[i]);
	int64_t n = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}
} // namespace

double GraphStore::getReadyRatio(uint64_t project_id, const char *ready_field)
{
	// Whitelist allowed field names to prevent SQL injection
	static const std::unordered_set<std::string> allowed_fields = {
		"fast_ready",	 "normal_ready",   "deep_ready",
		"fts_ready",	 "vector_ready",   "callgraph_ready",
		"metrics_ready", "embedding_ready"
	};
	if (!ready_field ||
	    allowed_fields.find(ready_field) == allowed_fields.end()) {
		return 0.0;
	}
	if (!db_)
		return 0.0;

	const int64_t pid = static_cast<int64_t>(project_id);
	const std::string field(ready_field);

	// Per-entity coverage: ratio of the eligible population that carries the
	// producer's output.
	const char *probe = nullptr;
	std::vector<int64_t> binds;
	if (field == "callgraph_ready") {
		probe = kReadyCallgraphSql;
		binds = { pid, pid };
	} else if (field == "metrics_ready") {
		probe = kReadyMetricsSql;
		binds = { pid };
	} else if (field == "embedding_ready" || field == "vector_ready") {
		probe = kReadyEmbeddingSql;
		binds = { pid };
	}
	if (probe) {
		const int64_t eligible =
			readyCount(db_, kReadyEligibleSql, { pid });
		const int64_t ready = readyCount(db_, probe, binds);
		if (eligible < 0 || ready < 0) {
			fprintf(stderr,
				"store: readiness probe failed for %s "
				"[module=store, method=getReadyRatio]\n",
				ready_field);
			return 0.0;
		}
		if (eligible == 0)
			return 0.0;
		return static_cast<double>(ready) /
		       static_cast<double>(eligible);
	}

	// The remaining fields are pipeline-stage FLAGS, not per-entity coverage:
	// project_readiness records whether the producer ran, which is what its
	// schema comment says it is for (see engine_get_enhancement_status for the
	// canonical count). No per-entity probe exists for them, so the ratio is
	// 1.0 or 0.0.
	return getProjectReadiness(project_id, ready_field) > 0 ? 1.0 : 0.0;
}
std::unordered_set<std::string>
GraphStore::loadFileScanStateBatch(uint64_t project_id)
{
	std::unordered_set<std::string> result;
	// M2: also read content_hash so the incremental skip can verify that a
	// file with the same mtime+size really is byte-identical (closes the
	// "same size + same mtime but changed content" hole).
	const char *sql =
		"SELECT file_path, file_mtime, file_size, COALESCE(content_hash,'') "
		"FROM file_scan_state WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"store: loadFileScanStateBatch prepare failed: %s "
			"[module=store, method=loadFileScanStateBatch]\n",
			sqlite3_errmsg(db_));
		return result;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		int64_t mtime = sqlite3_column_int64(stmt, 1);
		int64_t fsize = sqlite3_column_int64(stmt, 2);
		const char *ch = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		if (fp) {
			// Two keys per row: the mtime|size gate (fast, stat-only) and
			// the mtime|size|hash gate (requires reading the file to hash
			// it). The detection code checks the gate first (cheap) and
			// only hashes when the gate matches, so unchanged files with
			// different mtime/size skip without being read.
			std::string base = std::string(fp) + "|" +
					   std::to_string(mtime) + "|" +
					   std::to_string(fsize);
			result.insert(base);
			if (ch && *ch)
				result.insert(base + "|" + std::string(ch));
		}
	}
	sqlite3_finalize(stmt);
	return result;
}
void GraphStore::cleanupStaleFiles(uint64_t project_id,
				   const std::vector<std::string> &active_files)
{
	// Nested SAVEPOINT instead of BEGIN IMMEDIATE/COMMIT: this runs inside
	// the index transaction, where a plain BEGIN fails silently and the
	// matching COMMIT would commit — and end — the caller's transaction.
	if (!exec("SAVEPOINT cleanup_stale_files")) {
		fprintf(stderr,
			"store: cleanupStaleFiles SAVEPOINT failed: %s "
			"[module=store, method=cleanupStaleFiles]\n",
			error_.c_str());
		return;
	}

	bool ok = true;

	// Create the temp table (idempotent) and clear it in one shot.
	if (!exec("CREATE TEMP TABLE IF NOT EXISTS _active_files "
		  "(path TEXT PRIMARY KEY)") ||
	    !exec("DELETE FROM _active_files")) {
		fprintf(stderr,
			"store: cleanupStaleFiles temp table setup failed: %s "
			"[module=store, method=cleanupStaleFiles]\n",
			error_.c_str());
		ok = false;
	}

	if (ok) {
		// Reuse a single prepared INSERT for all files (1 prepare vs N).
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(
			    db_,
			    "INSERT OR IGNORE INTO _active_files (path) "
			    "VALUES (?)",
			    -1, &stmt, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"store: cleanupStaleFiles prepare insert "
				"failed: %s "
				"[module=store, method=cleanupStaleFiles]\n",
				sqlite3_errmsg(db_));
			ok = false;
		} else {
			for (const auto &f : active_files) {
				// SQLITE_STATIC: avoid SQLite internal memcpy
				// (caller owns the string for the duration of
				// the step call).
				sqlite3_bind_text(stmt, 1, f.c_str(),
						  static_cast<int>(f.size()),
						  SQLITE_STATIC);
				if (sqlite3_step(stmt) != SQLITE_DONE) {
					fprintf(stderr,
						"store: cleanupStaleFiles "
						"insert failed: %s "
						"[module=store, method="
						"cleanupStaleFiles]\n",
						sqlite3_errmsg(db_));
					ok = false;
					sqlite3_reset(stmt);
					break;
				}
				sqlite3_reset(stmt);
			}
			sqlite3_finalize(stmt);
		}
	}

	// Delete stale file_scan_state entries in one shot.
	if (ok) {
		sqlite3_stmt *del = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "DELETE FROM file_scan_state "
				       "WHERE project_id=? AND file_path NOT IN "
				       "(SELECT path FROM _active_files)",
				       -1, &del, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"store: cleanupStaleFiles prepare delete "
				"failed: %s "
				"[module=store, method=cleanupStaleFiles]\n",
				sqlite3_errmsg(db_));
			ok = false;
		} else {
			sqlite3_bind_int64(del, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(del) != SQLITE_DONE) {
				fprintf(stderr,
					"store: cleanupStaleFiles delete "
					"failed: %s "
					"[module=store, method="
					"cleanupStaleFiles]\n",
					sqlite3_errmsg(db_));
				ok = false;
			}
			sqlite3_finalize(del);
		}
	}

	if (!ok) {
		exec("ROLLBACK TO SAVEPOINT cleanup_stale_files");
		exec("RELEASE SAVEPOINT cleanup_stale_files");
		return;
	}

	// Drop temp table (kept for now to match existing behavior; could
	// be retained across calls with a TRUNCATE pattern for further savings).
	sqlite3_exec(db_, "DROP TABLE IF EXISTS _active_files", nullptr,
		     nullptr, nullptr);

	if (!exec("RELEASE SAVEPOINT cleanup_stale_files")) {
		fprintf(stderr,
			"store: cleanupStaleFiles RELEASE SAVEPOINT failed: %s "
			"[module=store, method=cleanupStaleFiles]\n",
			error_.c_str());
		exec("ROLLBACK TO SAVEPOINT cleanup_stale_files");
	}
}

// ─── Interactive Function Exploration ──────────────────────────

} // namespace store
