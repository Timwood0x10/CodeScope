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
#include "../query/vector_search.h"

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
		"INSERT INTO symbols "
		"(project_id, module_id, kind, name, signature, visibility, "
		" language, file_path, line, column, span_start, span_end) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertSymbol: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (module_id > 0)
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(module_id));
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, signature, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, visibility, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 7, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 9, line);
	sqlite3_bind_int(stmt, 10, column);
	sqlite3_bind_int(stmt, 11, span_start);
	sqlite3_bind_int(stmt, 12, span_end);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertSymbol: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);

	// Auto-create symbol_status row (defaults all to 0)
	{
		const char *ss_sql =
			"INSERT OR IGNORE INTO symbol_status (symbol_id) VALUES (?)";
		sqlite3_stmt *ss_stmt = nullptr;
		if (sqlite3_prepare_v2(db_, ss_sql, -1, &ss_stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(ss_stmt, 1,
					   static_cast<int64_t>(id));
			sqlite3_step(ss_stmt);
			sqlite3_finalize(ss_stmt);
		}
	}

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

	// Build tree: find roots (parent_id == 0), then output nested children
	std::ostringstream json;
	json << "{\"modules\":[";
	bool first = true;
	// Build child map: parent_id → list of child module ids
	std::unordered_map<uint64_t, std::vector<uint64_t> > children_of;
	for (const auto &m : modules)
		children_of[m.parent_id].push_back(m.id);
	std::function<void(uint64_t, int)> outMod = [&](uint64_t id,
							int depth) {
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
		     << ",\"language\":\"" << jsonEscape(it->language) << "\""
		     << ",\"file_count\":" << it->file_count;
		auto ci = children_of.find(id);
		if (ci != children_of.end() && !ci->second.empty()) {
			json << ",\"children\":[";
			bool cf = true;
			for (auto cid : ci->second) {
				if (!cf)
					json << ",";
				cf = false;
				outMod(cid, depth + 1);
			}
			json << "]";
		}
		json << "}";
	};
	for (const auto &m : modules)
		if (m.parent_id == 0)
			outMod(m.id, 0);
	json << "]}";
	return json.str();
}

std::string GraphStore::findSymbolJson(uint64_t project_id, const char *name)
{
	const char *sql =
		"SELECT id, module_id, kind, name, signature, visibility, "
		"language, file_path, line, column "
		"FROM symbols WHERE project_id = ? AND name = ?";
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
			sqlite3_column_text(stmt, 2));
		const char *sym_name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		const char *sig = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 4));
		const char *vis = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 5));
		const char *lang = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 6));
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 7));
		int line = sqlite3_column_int(stmt, 8);
		int col = sqlite3_column_int(stmt, 9);

		json << "{"
		     << "\"id\":" << id << ","
		     << "\"kind\":\"" << (kind ? kind : "") << "\","
		     << "\"name\":\"" << jsonEscape(sym_name ? sym_name : "")
		     << "\","
		     << "\"signature\":\"" << jsonEscape(sig ? sig : "")
		     << "\","
		     << "\"visibility\":\"" << (vis ? vis : "") << "\","
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

// ── Phase B: Enhancement — Call Edges ─────────────────────────

uint64_t GraphStore::insertCallEdge(uint64_t project_id,
				    uint64_t caller_symbol_id,
				    uint64_t callee_symbol_id,
				    const char *provenance, int line, int col)
{
	const char *sql =
		"INSERT OR IGNORE INTO call_edges "
		"(project_id, caller_symbol_id, callee_symbol_id, provenance, line, col) "
		"VALUES (?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(caller_symbol_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(callee_symbol_id));
	sqlite3_bind_text(stmt, 4, provenance ? provenance : "static", -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 5, line);
	sqlite3_bind_int(stmt, 6, col);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertCallEdge: step failed";
		return 0;
	}
	// Return 0 when the row was ignored (duplicate); non-zero row id otherwise.
	// Callers use this to decide whether to increment the edge counter.
	if (sqlite3_changes(db_) == 0)
		return 0;
	return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

uint64_t GraphStore::insertDependencyEdge(uint64_t project_id,
					  uint64_t source_module_id,
					  uint64_t target_module_id,
					  const char *external_name,
					  const char *kind)
{
	const char *sql =
		"INSERT INTO dependency_edges "
		"(project_id, source_module_id, target_module_id, external_name, kind) "
		"VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		error_ = "insertDependencyEdge: prepare failed";
		return 0;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(source_module_id));
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(target_module_id));
	sqlite3_bind_text(stmt, 4, external_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, kind, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertDependencyEdge: step failed";
		sqlite3_finalize(stmt);
		return 0;
	}
	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	sqlite3_finalize(stmt);
	return id;
}

// ── Phase B: Enhancement — Metrics ────────────────────────────

bool GraphStore::insertMetric(uint64_t project_id, const char *owner_type,
			      uint64_t owner_id, int cyclomatic,
			      int nesting_depth, int cognitive, int lines,
			      int param_count, int call_count, int branch_count,
			      int loop_count)
{
	const char *sql = "INSERT OR REPLACE INTO metrics "
			  "(project_id, owner_type, owner_id, cyclomatic, "
			  "nesting_depth, cognitive, lines, "
			  " param_count, call_count, branch_count, loop_count) "
			  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, owner_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(owner_id));
	sqlite3_bind_int(stmt, 4, cyclomatic);
	sqlite3_bind_int(stmt, 5, nesting_depth);
	sqlite3_bind_int(stmt, 6, cognitive);
	sqlite3_bind_int(stmt, 7, lines);
	sqlite3_bind_int(stmt, 8, param_count);
	sqlite3_bind_int(stmt, 9, call_count);
	sqlite3_bind_int(stmt, 10, branch_count);
	sqlite3_bind_int(stmt, 11, loop_count);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertMetric: step failed";
		return false;
	}
	return true;
}

// ── Phase B: Enhancement — Search Index ───────────────────────

bool GraphStore::insertIntoSearchIndex(uint64_t symbol_id, uint64_t project_id,
				       const char *title, const char *summary,
				       const char *body)
{
	const char *sql =
		"INSERT INTO search_index (symbol_id, project_id, title, summary, body) "
		"VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, title ? title : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, summary ? summary : "", -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, body ? body : "", -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "insertIntoSearchIndex: step failed";
		return false;
	}
	return true;
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
			    db_, "SELECT project_id FROM ir_nodes WHERE id=?",
			    -1, &pid_st, nullptr) == SQLITE_OK) {
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

	// Also write to the vec0 embeddings table if available.
	// Returns false when the vec0 INSERT fails (table missing or step error)
	// so the caller can skip markEmbeddingReady and self-heal on rerun.
	// node_vectors was already written above, so searchSemantic still works
	// via graceful degradation even when vec0 is unavailable.
	const char *sql =
		"INSERT INTO embeddings (symbol_id, vector) VALUES (?, ?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		// embeddings table not available — vec0 extension not loaded
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	sqlite3_bind_blob(stmt, 2, vec.data(),
			  TARGET_DIM * static_cast<int>(sizeof(float)),
			  SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		// embeddings INSERT failed — node_vectors has the data, but
		// vec0 is unavailable so embedding_ready must stay 0 for rerun.
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

// ── Phase B: Enhancement — Ready Flags ────────────────────────

bool GraphStore::markCallgraphAndMetricsReady(uint64_t symbol_id)
{
	const char *sql =
		"UPDATE symbol_status SET callgraph_ready=1, metrics_ready=1 "
		"WHERE symbol_id = ?";
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
	const char *sql =
		"UPDATE symbol_status SET embedding_ready=1 WHERE symbol_id = ?";
	sqlite3_stmt *stmt = getCachedStmt(sql);
	if (!stmt) {
		return false;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(symbol_id));
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		error_ = "markEmbeddingReady: step failed";
		return false;
	}
	return true;
}

bool GraphStore::setSymbolStub(uint64_t symbol_id, bool is_stub)
{
	const char *sql =
		"UPDATE symbol_status SET is_stub = ? WHERE symbol_id = ?";
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

	std::string sql = "SELECT DISTINCT s.file_path FROM symbols s "
			  "JOIN symbol_status ss ON ss.symbol_id = s.id "
			  "WHERE s.project_id = ? AND ss." +
			  std::string(ready_field) +
			  " = 0 "
			  "ORDER BY s.file_path";
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
			  "CAST(SUM(ss." +
			  std::string(ready_field) +
			  ") AS REAL) / COUNT(*) "
			  "ELSE 0.0 END FROM symbols s "
			  "JOIN symbol_status ss ON ss.symbol_id = s.id "
			  "WHERE s.project_id = ?";
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
	// Build temp table of active files for efficient set-difference
	const char *create_sql =
		"CREATE TEMP TABLE IF NOT EXISTS _active_files (path TEXT PRIMARY KEY)";
	sqlite3_exec(db_, create_sql, nullptr, nullptr, nullptr);

	sqlite3_stmt *stmt = nullptr;

	// Clear previous active files
	sqlite3_prepare_v2(db_, "DELETE FROM _active_files", -1, &stmt,
			   nullptr);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// Insert current active files
	sqlite3_prepare_v2(db_, "INSERT INTO _active_files (path) VALUES (?)",
			   -1, &stmt, nullptr);
	for (const auto &f : active_files) {
		sqlite3_bind_text(stmt, 1, f.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_reset(stmt);
	}
	sqlite3_finalize(stmt);

	// Delete stale file_scan_state entries (files that no longer exist)
	sqlite3_prepare_v2(db_,
			   "DELETE FROM file_scan_state "
			   "WHERE project_id=? AND file_path NOT IN "
			   "(SELECT path FROM _active_files)",
			   -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// Drop temp table
	sqlite3_exec(db_, "DROP TABLE IF EXISTS _active_files", nullptr,
		     nullptr, nullptr);
}

// ─── Interactive Function Exploration ──────────────────────────

} // namespace store
