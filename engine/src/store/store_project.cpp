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

// ── New Schema (Phase A): Symbols ─────────────────────────────

uint64_t GraphStore::insertSymbol(uint64_t project_id, uint64_t module_id,
				  const char *kind, const char *name,
				  const char *signature, const char *visibility,
				  const char *language, const char *file_path,
				  int line, int column, int span_start,
				  int span_end)
{
	const char *sql =
		"INSERT OR IGNORE INTO graph_nodes "
		"(project_id, node_type, name, signature, file_path, language, "
		" start_row, start_col) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertSymbol: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	// NOTE: module_id is intentionally NOT bound here — graph_nodes has no
	// module_id column (see the INSERT column list above:
	// project_id, node_type, name, signature, file_path, language,
	// start_row, start_col). The removed code bound ?2 to module_id and then
	// OVERWROTE it with node_type, so ?2 always ended up as node_type (the
	// correct value) but module_id was silently dropped. ?2 belongs to
	// node_type only. (void)module_id; below documents the unused parameter.
	// Map string kind to node_type integer
	int node_type = 7;
	if (kind) {
		std::string ks = kind;
		if (ks == "function")
			node_type = 0;
		else if (ks == "method")
			node_type = 1;
		else if (ks == "class")
			node_type = 2;
		else if (ks == "struct")
			node_type = 3;
		else if (ks == "interface")
			node_type = 4;
		else if (ks == "enum")
			node_type = 5;
		else if (ks == "type_alias")
			node_type = 6;
	}
	(void)module_id;
	(void)visibility;
	(void)span_start;
	(void)span_end;
	sqlite3_bind_int(stmt, 2, node_type);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, signature, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, line);
	sqlite3_bind_int(stmt, 8, column);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertSymbol: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);

	return id;
}

// ── New Schema (Phase A): Entry Points ────────────────────────

bool GraphStore::insertEntryPoint(uint64_t symbol_id, uint64_t project_id,
				  const char *kind)
{
	const char *sql =
		"INSERT OR REPLACE INTO entry_points (symbol_id, project_id, kind) "
		"VALUES (?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertEntryPoint: prepare failed";
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertEntryPoint: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
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
			json << "{\"id\":" << it->id
			     << ",\"parent_id\":" << it->parent_id
			     << ",\"depth\":" << depth << ",\"name\":\""
			     << jsonEscape(it->name) << "\""
			     << ",\"path\":\"" << jsonEscape(it->path) << "\""
			     << ",\"language\":\"" << jsonEscape(it->language)
			     << "\""
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

		json << "{"
		     << "\"id\":" << id << ","
		     << "\"kind\":\"" << (kind ? kind : "") << "\","
		     << "\"name\":\"" << jsonEscape(sym_name ? sym_name : "")
		     << "\","
		     << "\"signature\":\"" << jsonEscape(sig ? sig : "")
		     << "\","
		     << "\"language\":\"" << (lang ? lang : "") << "\","
		     << "\"file_path\":\"" << jsonEscape(fp ? fp : "") << "\","
		     << "\"line\":" << line << ","
		     << "\"column\":" << col << "}";
	}
	json << "]}";

	// If symbols table was empty, fall back to graph_nodes (new pipeline)
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
				gn_json << "{"
					<< "\"id\":" << id << ","
					<< "\"kind\":\"" << type_name << "\","
					<< "\"name\":\""
					<< jsonEscape(n ? n : "") << "\","
					<< "\"file_path\":\""
					<< jsonEscape(fp ? fp : "") << "\","
					<< "\"line\":" << sr << ","
					<< "\"column\":" << sc << ","
					<< "\"language\":\""
					<< (lang ? lang : "") << "\""
					<< "}";
			}
			sqlite3_finalize(gn_stmt);
		}
		gn_json << "]}";
		return gn_json.str();
	}

	return json.str();
}

// ── Phase B: Enhancement — Embeddings ─────────────────────────

bool GraphStore::insertEmbedding(uint64_t symbol_id, const float *vector_data,
				 int dim)
{
	// Guard against negative dim (would wrap to a huge size_t in the
	// memcpy below and cause a heap buffer overflow) and null input.
	if (dim <= 0 || !vector_data)
		return false;

	// The vec0 table expects a blob of float32 values
	// Pad/truncate to 384 (schema definition)
	constexpr int TARGET_DIM = 384;
	std::vector<float> vec(TARGET_DIM, 0.0f);
	int copy_dim = dim < TARGET_DIM ? dim : TARGET_DIM;
	memcpy(vec.data(), vector_data,
	       static_cast<size_t>(copy_dim) * sizeof(float));

	// Write to node_vectors FIRST — this is the table that searchSemantic
	// actually reads from (store.cpp ~771). This always works because
	// node_vectors is a regular SQLite table, not a vec0 virtual table.
	// On platforms where vec0.dll isn't available (e.g. Windows without
	// the extension), the embeddings INSERT below will fail, but the
	// node_vectors path still succeeds, providing graceful degradation.
	{
		uint64_t proj_id = 0;
		sqlite3_stmt *pid_st = nullptr;
		if (sqlite3_prepare_v2(
			    db_,
			    "SELECT project_id FROM graph_nodes WHERE id=?", -1,
			    &pid_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(pid_st, 1,
					   static_cast<int64_t>(symbol_id));
			if (sqlite3_step(pid_st) == SQLITE_ROW)
				proj_id = static_cast<uint64_t>(
					sqlite3_column_int64(pid_st, 0));
			sqlite3_finalize(pid_st);
		}
		if (proj_id > 0 && stmt_vector_) {
			sqlite3_reset(stmt_vector_);
			sqlite3_bind_int64(stmt_vector_, 1,
					   static_cast<int64_t>(symbol_id));
			sqlite3_bind_int64(stmt_vector_, 2,
					   static_cast<int64_t>(proj_id));
			sqlite3_bind_blob(
				stmt_vector_, 3, vec.data(),
				TARGET_DIM * static_cast<int>(sizeof(float)),
				SQLITE_TRANSIENT);
			sqlite3_step(stmt_vector_);
		}
	}

	// node_vectors was already written above (the only vector storage).
	// The vec0 embeddings table was removed — searchSemantic reads
	// node_vectors directly.
	return true;
}

// ── Phase B: Enhancement — Ready Flags ────────────────────────
//
// Step 10 (sunset): these setters target the deprecated `graph_nodes` table
// (which is empty in the canonical schema — `entity`/`relation` are the
// source of truth). They are kept as dead-code defensive seams so any future
// caller that tries to mark metrics/embedding ready cannot accidentally flip
// the flag without real data backing it. metrics_ready is structurally 0
// (metrics producer sunset); embedding_ready is structurally 0 (vector
// builder sunset). Only callgraph_ready is a legitimate signal, and even
// that is now set via the canonical path in engine_index_post_parse.cpp.

bool GraphStore::markCallgraphAndMetricsReady(uint64_t symbol_id)
{
	// Sunset: do NOT set metrics_ready=1 — there is no metrics data.
	// Setting it here would re-introduce the A18 "fake ready" bug. Only
	// callgraph_ready is set, and only on the deprecated graph_nodes row
	// (canonical callgraph readiness is computed from relation.type=1
	// coverage in engine_get_enhancement_status / engine_get_capabilities).
	const char *sql =
		"UPDATE graph_nodes SET callgraph_ready=1 WHERE id = ?";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "markCallgraphAndMetricsReady: step failed";
		return false;
	}
	return true;
}

bool GraphStore::markEmbeddingReady(uint64_t symbol_id)
{
	// Sunset: embedding producer (buildVectorsFromGraph) is a no-op, so
	// node_vectors stays empty. Setting embedding_ready=1 here would
	// re-introduce the A19 "fake ready" bug. The flag is structurally 0
	// and canonical embedding readiness is computed from node_vectors
	// row count in engine_get_enhancement_status / engine_get_capabilities.
	// This function is retained as a no-op defensive seam so any future
	// caller cannot silently flip the flag.
	(void)symbol_id;
	return true;
}

bool GraphStore::setSymbolStub(uint64_t symbol_id, bool is_stub)
{
	const char *sql = "UPDATE graph_nodes SET is_stub = ? WHERE id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "setSymbolStub: prepare failed";
		return false;
	}
	sqlite3_bind_int(stmt, 1, is_stub ? 1 : 0);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(symbol_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "setSymbolStub: step failed";
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

std::vector<std::string> GraphStore::getUnreadyFiles(uint64_t project_id,
						     const char *ready_field)
{
	// Whitelist allowed field names to prevent SQL injection
	static const std::unordered_set<std::string> allowed_fields = {
		"fast_ready",	 "normal_ready",   "deep_ready",
		"fts_ready",	 "vector_ready",   "callgraph_ready",
		"metrics_ready", "embedding_ready"
	};
	if (!ready_field ||
	    allowed_fields.find(ready_field) == allowed_fields.end()) {
		return {};
	}

	std::string sql = "SELECT DISTINCT gn.file_path FROM graph_nodes gn "
			  "WHERE gn.project_id = ? AND gn." +
			  std::string(ready_field) +
			  " = 0 "
			  "ORDER BY gn.file_path";
	sqlite3_stmt *stmt = nullptr;
	std::vector<std::string> files;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		error_ = "getUnreadyFiles: prepare failed";
		return files;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (fp)
			files.emplace_back(fp);
	}
	sqlite3_finalize(stmt);
	return files;
}

// ── Phase C: Unified Queries ──────────────────────────────────

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

	std::string sql = "SELECT CASE WHEN COUNT(*) > 0 THEN "
			  "CAST(SUM(gn." +
			  std::string(ready_field) +
			  ") AS REAL) / COUNT(*) "
			  "ELSE 0.0 END FROM graph_nodes gn "
			  "WHERE gn.project_id = ? AND gn.node_type IN (0,1)";
	sqlite3_stmt *stmt = nullptr;
	double ratio = 0.0;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			ratio = sqlite3_column_double(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	return ratio;
}

// ── Index Tasks (Tokio background task tracking) ────────────

bool GraphStore::isFileUnchanged(uint64_t project_id, const char *file_path,
				 int64_t mtime, int64_t size)
{
	const char *sql =
		"SELECT 1 FROM file_scan_state WHERE project_id=? AND file_path=? AND file_mtime=? AND file_size=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, mtime);
	sqlite3_bind_int64(stmt, 4, size);
	bool unchanged = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return unchanged;
}

std::unordered_set<std::string>
GraphStore::loadFileScanStateBatch(uint64_t project_id)
{
	std::unordered_set<std::string> result;
	const char *sql =
		"SELECT file_path, file_mtime, file_size FROM file_scan_state WHERE project_id=?";
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
		if (fp) {
			std::string key = std::string(fp) + "|" +
					  std::to_string(mtime) + "|" +
					  std::to_string(fsize);
			result.insert(std::move(key));
		}
	}
	sqlite3_finalize(stmt);
	return result;
}

void GraphStore::updateFileScanState(uint64_t project_id, const char *file_path,
				     int64_t mtime, int64_t size)
{
	const char *sql =
		"INSERT OR REPLACE INTO file_scan_state (project_id, file_path, file_mtime, file_size) VALUES (?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, mtime);
	sqlite3_bind_int64(stmt, 4, size);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void GraphStore::cleanupStaleFiles(uint64_t project_id,
				   const std::vector<std::string> &active_files)
{
	// Wrap in a transaction for atomicity + throughput (1 commit vs N).
	exec("BEGIN IMMEDIATE");

	// Create temp table if not exists (idempotent).
	sqlite3_exec(
		db_,
		"CREATE TEMP TABLE IF NOT EXISTS _active_files (path TEXT PRIMARY KEY)",
		nullptr, nullptr, nullptr);

	// Clear previous active files in one shot.
	sqlite3_exec(db_, "DELETE FROM _active_files", nullptr, nullptr,
		     nullptr);

	// Reuse a single prepared INSERT for all files (1 prepare vs N prepares).
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(
		    db_,
		    "INSERT OR IGNORE INTO _active_files (path) VALUES (?)", -1,
		    &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"store: cleanupStaleFiles prepare insert failed: %s "
			"[module=store, method=cleanupStaleFiles]\n",
			sqlite3_errmsg(db_));
		exec("ROLLBACK");
		return;
	}
	for (const auto &f : active_files) {
		// SQLITE_STATIC: avoid SQLite internal memcpy (caller owns the
		// string for the duration of the step call).
		sqlite3_bind_text(stmt, 1, f.c_str(),
				  static_cast<int>(f.size()), SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_reset(stmt);
	}
	sqlite3_finalize(stmt);

	// Delete stale file_scan_state entries in one shot.
	{
		sqlite3_stmt *del = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "DELETE FROM file_scan_state "
				       "WHERE project_id=? AND file_path NOT IN "
				       "(SELECT path FROM _active_files)",
				       -1, &del, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(del, 1,
					   static_cast<int64_t>(project_id));
			sqlite3_step(del);
			sqlite3_finalize(del);
		} else {
			fprintf(stderr,
				"store: cleanupStaleFiles prepare delete failed: %s "
				"[module=store, method=cleanupStaleFiles]\n",
				sqlite3_errmsg(db_));
		}
	}

	// Drop temp table (kept for now to match existing behavior; could
	// be retained across calls with a TRUNCATE pattern for further savings).
	sqlite3_exec(db_, "DROP TABLE IF EXISTS _active_files", nullptr,
		     nullptr, nullptr);

	exec("COMMIT");
}

// ─── Interactive Function Exploration ──────────────────────────

} // namespace store
