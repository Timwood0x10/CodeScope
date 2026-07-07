#include "store.h"

#include "posix_compat.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace store
{

// ─── Lifecycle ─────────────────────────────────────────────────

// Performance PRAGMA values
static constexpr int kCacheSizePages = -64000; // 64 MB cache
static constexpr int kMmapSizeBytes = 268435456; // 256 MB mmap

GraphStore::~GraphStore()
{
	close();
}

bool GraphStore::open(const char *db_path)
{
	int rc = sqlite3_open(db_path, &db_);
	if (rc != SQLITE_OK) {
		error_ = sqlite3_errmsg(db_);
		return false;
	}

	// Performance PRAGMAs: WAL mode + MEMORY temp + synchronous OFF.
	// WARNING: synchronous=OFF means fsync is never called — a power failure
	// or crash may corrupt the database. Acceptable for CodeScope since the
	// DB is a cache that can be rebuilt by re-indexing. Use synchronous=NORMAL
	// if data safety is more important than write performance.
	if (!exec("PRAGMA journal_mode=WAL"))
		fprintf(stderr, "WARN: PRAGMA journal_mode=WAL failed: %s\n",
			error_.c_str());
	if (!exec("PRAGMA synchronous=OFF"))
		fprintf(stderr, "WARN: PRAGMA synchronous=OFF failed\n");
	if (!exec("PRAGMA temp_store=MEMORY"))
		fprintf(stderr, "WARN: PRAGMA temp_store=MEMORY failed\n");
	exec(("PRAGMA cache_size=" + std::to_string(kCacheSizePages)).c_str());
	exec(("PRAGMA mmap_size=" + std::to_string(kMmapSizeBytes)).c_str());

	if (!createSchema())
		return false;

	// Pre-cache prepared statements for hot insert paths
	sqlite3_prepare_v2(
		db_,
		"INSERT OR REPLACE INTO fts_node_map (node_id, project_id, file_id) "
		"VALUES (?, ?, 0)",
		-1, &stmt_fts_map_, nullptr);
	sqlite3_prepare_v2(
		db_,
		"INSERT OR REPLACE INTO code_fts (rowid, name, qualified_name, "
		"file_path, content, project_id, node_id, node_kind) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
		-1, &stmt_fts_, nullptr);
	sqlite3_prepare_v2(
		db_,
		"INSERT OR REPLACE INTO node_vectors (node_id, project_id, vector) "
		"VALUES (?, ?, ?)",
		-1, &stmt_vector_, nullptr);

	return true;
}

void GraphStore::close()
{
	if (db_) {
		// Finalize cached prepared statements
		if (stmt_fts_map_)
			sqlite3_finalize(stmt_fts_map_);
		if (stmt_fts_)
			sqlite3_finalize(stmt_fts_);
		if (stmt_vector_)
			sqlite3_finalize(stmt_vector_);
		stmt_fts_map_ = stmt_fts_ = stmt_vector_ = nullptr;

		sqlite3_close(db_);
		db_ = nullptr;
	}
}

// ─── Schema ────────────────────────────────────────────────────

bool GraphStore::createSchema()
{
	const char *schema = R"SQL(
        CREATE TABLE IF NOT EXISTS projects (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            root_path TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            created_at TEXT DEFAULT (datetime('now'))
        );

        -- Project readiness: tracks which index phases have completed.
        -- Each column is 0 (not ready) or 1 (ready).
        CREATE TABLE IF NOT EXISTS project_readiness (
            project_id INTEGER PRIMARY KEY,
            fast_ready INTEGER DEFAULT 0,
            normal_ready INTEGER DEFAULT 0,
            deep_ready INTEGER DEFAULT 0,
            fts_ready INTEGER DEFAULT 0,
            vector_ready INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            path TEXT NOT NULL,
            language TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            last_parsed_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id),
            UNIQUE(project_id, path)
        );

        CREATE TABLE IF NOT EXISTS ir_nodes (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            file_id INTEGER NOT NULL,
            parent_id INTEGER,
            kind INTEGER NOT NULL,
            name TEXT,
            qualified_name TEXT,
            start_row INTEGER NOT NULL, start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL, end_col INTEGER NOT NULL,
            language TEXT NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (file_id) REFERENCES files(id)
        );

        CREATE TABLE IF NOT EXISTS ir_semantic_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_node_id INTEGER NOT NULL,
            target_node_id INTEGER NOT NULL,
            relation INTEGER NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_node_id) REFERENCES ir_nodes(id),
            FOREIGN KEY (target_node_id) REFERENCES ir_nodes(id)
        );

        CREATE TABLE IF NOT EXISTS graph_nodes (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            ir_node_id INTEGER NOT NULL,
            node_type INTEGER NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT,
            module_path TEXT DEFAULT '',
            package_name TEXT DEFAULT '',
            class_name TEXT DEFAULT '',
            start_row INTEGER NOT NULL, start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL, end_col INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            language TEXT NOT NULL,
            signature TEXT DEFAULT '',
            complexity INTEGER DEFAULT 0,
            is_entry_point INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS graph_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_node_id INTEGER NOT NULL,
            target_node_id INTEGER NOT NULL,
            edge_type INTEGER NOT NULL,
            graph_type TEXT NOT NULL DEFAULT 'symbol_reference',
            call_site_file TEXT DEFAULT '',
            call_site_line INTEGER DEFAULT 0,
            label TEXT DEFAULT '',
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_node_id) REFERENCES graph_nodes(id),
            FOREIGN KEY (target_node_id) REFERENCES graph_nodes(id)
        );

        CREATE INDEX IF NOT EXISTS idx_files_project ON files(project_id);
        CREATE INDEX IF NOT EXISTS idx_ir_nodes_project ON ir_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_ir_nodes_file ON ir_nodes(project_id, file_id);
        CREATE INDEX IF NOT EXISTS idx_ir_nodes_name ON ir_nodes(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_ir_edges_src ON ir_semantic_edges(source_node_id);
        CREATE INDEX IF NOT EXISTS idx_ir_edges_tgt ON ir_semantic_edges(target_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_project ON graph_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id);
        -- Composite indexes for caller/callee queries: edge_type + node_id
        -- getCallers: WHERE edge_type=1 AND target_node_id IN (SELECT id FROM graph_nodes WHERE name=?)
        -- getCallees: WHERE edge_type=1 AND source_node_id IN (SELECT id FROM graph_nodes WHERE name=?)
        CREATE INDEX IF NOT EXISTS idx_ge_callers ON graph_edges(edge_type, target_node_id);
        CREATE INDEX IF NOT EXISTS idx_ge_callees ON graph_edges(edge_type, source_node_id);

        -- Semantic records table (flat, O(1) parse-time memory)
        -- Uses AUTOINCREMENT rowid to avoid per-file ID conflicts (each file's
        -- record IDs start at 1). original_id stores the per-file record ID.
        CREATE TABLE IF NOT EXISTS semantic_records (
            rowid INTEGER PRIMARY KEY AUTOINCREMENT,
            original_id INTEGER NOT NULL,
            project_id INTEGER NOT NULL,
            kind INTEGER NOT NULL,
            name TEXT,
            qualified_name TEXT DEFAULT '',
            parent_id INTEGER DEFAULT 0,
            start_row INTEGER DEFAULT 0, start_col INTEGER DEFAULT 0,
            end_row INTEGER DEFAULT 0, end_col INTEGER DEFAULT 0,
            file_path TEXT NOT NULL,
            language TEXT DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_sr_project ON semantic_records(project_id);
        CREATE INDEX IF NOT EXISTS idx_sr_parent ON semantic_records(project_id, parent_id);
        CREATE INDEX IF NOT EXISTS idx_sr_name ON semantic_records(project_id, name);
        -- Indexes for buildGraph JOINs: (project_id, file_path) for file filter,
        -- (file_path, original_id) for containment edges, (project_id, kind) for declaration filter
        CREATE INDEX IF NOT EXISTS idx_sr_file ON semantic_records(project_id, file_path);
        CREATE INDEX IF NOT EXISTS idx_sr_file_oid ON semantic_records(file_path, original_id);
        CREATE INDEX IF NOT EXISTS idx_sr_kind ON semantic_records(project_id, kind);
        -- Index for containment edges parent JOIN: (file_path, parent_id)
        CREATE INDEX IF NOT EXISTS idx_sr_fp_parent ON semantic_records(file_path, parent_id);
        -- Index for call edges name matching: (project_id, kind, name) covers the WHERE + JOIN
        CREATE INDEX IF NOT EXISTS idx_sr_kind_name ON semantic_records(project_id, kind, name);

        -- FTS5 full-text search index
        CREATE VIRTUAL TABLE IF NOT EXISTS code_fts USING fts5(
            name, qualified_name, file_path, content,
            project_id UNINDEXED,
            node_id UNINDEXED,
            node_kind UNINDEXED,
            tokenize='unicode61'
        );

        CREATE TABLE IF NOT EXISTS fts_node_map (
            node_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            file_id INTEGER NOT NULL
        );

        -- Code complexity scores per graph node
        CREATE TABLE IF NOT EXISTS node_complexity (
            project_id INTEGER NOT NULL,
            graph_node_id INTEGER NOT NULL,
            cyclomatic INTEGER NOT NULL DEFAULT 0,
            cognitive INTEGER NOT NULL DEFAULT 0,
            nesting_depth INTEGER NOT NULL DEFAULT 0,
            decision_points INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (project_id, graph_node_id),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- Semantic vector index (n-gram hash vectors for each ir_node)
        CREATE TABLE IF NOT EXISTS node_vectors (
            node_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            vector BLOB NOT NULL
        );

        -- ============================================================
        -- Phase A: Skeleton Index (facts, ms-level, one schema)
        -- ============================================================

        -- modules: path is stored as project-relative for portability
        CREATE TABLE IF NOT EXISTS modules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            parent_id INTEGER REFERENCES modules(id),
            name TEXT NOT NULL,
            path TEXT NOT NULL,          -- project-relative path
            language TEXT,
            file_count INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- symbols: lean main table, status in symbol_status
        CREATE TABLE IF NOT EXISTS symbols (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            module_id INTEGER REFERENCES modules(id),
            kind TEXT NOT NULL,          -- function/method/class/struct/trait/enum/const/type_alias
            name TEXT NOT NULL,
            signature TEXT,
            visibility TEXT DEFAULT 'default',
            language TEXT NOT NULL,
            file_path TEXT NOT NULL,     -- project-relative path
            line INTEGER NOT NULL,
            column INTEGER NOT NULL,
            span_start INTEGER,          -- byte offset
            span_end INTEGER,
            role TEXT DEFAULT 'unknown',  -- API/Callback/Driver/Hook/Entry/Utility/Test
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
        CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_symbols_module ON symbols(module_id);

        -- symbol_status: separate table for analysis progress flags, keeps symbols lean
        CREATE TABLE IF NOT EXISTS symbol_status (
            symbol_id INTEGER PRIMARY KEY,
            callgraph_ready INTEGER DEFAULT 0,
            metrics_ready INTEGER DEFAULT 0,
            embedding_ready INTEGER DEFAULT 0,
            is_stub INTEGER DEFAULT 0,
            FOREIGN KEY (symbol_id) REFERENCES symbols(id)
        );

        -- index_tasks: background task tracking for Tokio queue
        CREATE TABLE IF NOT EXISTS index_tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            task_type TEXT NOT NULL,     -- 'scan' / 'enhance' / 'embedding'
            status TEXT NOT NULL DEFAULT 'pending',  -- pending/running/completed/failed
            progress INTEGER DEFAULT 0,  -- 0-100
            error TEXT,
            started_at TEXT,
            completed_at TEXT,
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS entry_points (
            symbol_id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            kind TEXT NOT NULL,          -- main/init/setup/run/handler
            FOREIGN KEY (symbol_id) REFERENCES symbols(id),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- ============================================================
        -- Phase B: Knowledge Enhancement Tables
        -- ============================================================

        CREATE TABLE IF NOT EXISTS call_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            caller_symbol_id INTEGER NOT NULL,
            callee_symbol_id INTEGER NOT NULL,
            provenance TEXT DEFAULT 'static',  -- static/lsp/resolved
            line INTEGER,
            col INTEGER,
            FOREIGN KEY (caller_symbol_id) REFERENCES symbols(id),
            FOREIGN KEY (callee_symbol_id) REFERENCES symbols(id)
        );
        CREATE INDEX IF NOT EXISTS idx_call_edges_caller ON call_edges(caller_symbol_id);
        CREATE INDEX IF NOT EXISTS idx_call_edges_callee ON call_edges(callee_symbol_id);

        -- Dependency edges: module-level, supports external/third-party deps
        CREATE TABLE IF NOT EXISTS dependency_edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_module_id INTEGER REFERENCES modules(id),
            target_module_id INTEGER REFERENCES modules(id),
            external_name TEXT,          -- e.g. "tokio::sync::Mutex", "context"
            kind TEXT NOT NULL,          -- import/include/inherit/implement/use/ffi
            FOREIGN KEY (source_module_id) REFERENCES modules(id)
        );
        CREATE INDEX IF NOT EXISTS idx_dep_edges_src ON dependency_edges(source_module_id);
        CREATE INDEX IF NOT EXISTS idx_dep_edges_tgt ON dependency_edges(target_module_id);

        -- Metrics: generic owner_type/owner_id for symbols, modules, projects
        CREATE TABLE IF NOT EXISTS metrics (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            owner_type TEXT NOT NULL,    -- 'symbol' / 'module' / 'project'
            owner_id INTEGER NOT NULL,
            cyclomatic INTEGER DEFAULT 0,
            nesting_depth INTEGER DEFAULT 0,
            cognitive INTEGER DEFAULT 0,
            lines INTEGER DEFAULT 0,
            param_count INTEGER DEFAULT 0,
            call_count INTEGER DEFAULT 0,
            branch_count INTEGER DEFAULT 0,
            loop_count INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            UNIQUE(owner_type, owner_id)
        );
        CREATE INDEX IF NOT EXISTS idx_metrics_owner ON metrics(owner_type, owner_id);

        -- FTS5 search index: title/summary/body for better AI search
        CREATE VIRTUAL TABLE IF NOT EXISTS search_index USING fts5(
            title, summary, body,
            project_id UNINDEXED,
            symbol_id UNINDEXED,
            tokenize='unicode61'
        );

        -- Trigger: auto-clean search_index when symbols are deleted
        CREATE TRIGGER IF NOT EXISTS trg_symbols_delete AFTER DELETE ON symbols BEGIN
            DELETE FROM search_index WHERE symbol_id = old.id;
        END;

        -- file_scan_state: tracks file modification times for incremental indexing
        CREATE TABLE IF NOT EXISTS file_scan_state (
            project_id INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            file_mtime INTEGER NOT NULL,   -- last modification time (epoch seconds)
            file_size INTEGER NOT NULL DEFAULT 0,
            content_hash TEXT,             -- optional: hash for content change detection
            scanned_at TEXT DEFAULT (datetime('now')),
            PRIMARY KEY (project_id, file_path),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );
    )SQL";

	// Execute main schema
	bool ok = exec(schema);

	// Note: vec0 embeddings table is created in engine_init() after
	// sqlite-vec extension is loaded via dlopen. Not needed here.
	(void)ok;

	return ok;
}

bool GraphStore::createIndexesAfterBulkLoad(uint64_t project_id)
{
	(void)project_id; // All indexes are global — project_id not needed
	// Deferred indexes: created after bulk insert to avoid per-row index maintenance.
	// Query-time indexes (graph_edges, symbols, call_edges, etc.) don't need to exist
	// during bulk write — they only speed up user queries.
	const char *indexes[] = {
		"CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name)",
		"CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id)",
		"CREATE INDEX IF NOT EXISTS idx_ge_callers ON graph_edges(edge_type, target_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_ge_callees ON graph_edges(edge_type, source_node_id)",
		"CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(project_id, name)",
		"CREATE INDEX IF NOT EXISTS idx_call_edges_caller ON call_edges(caller_symbol_id)",
		"CREATE INDEX IF NOT EXISTS idx_call_edges_callee ON call_edges(callee_symbol_id)",
		"CREATE INDEX IF NOT EXISTS idx_dep_edges_src ON dependency_edges(source_module_id)",
		"CREATE INDEX IF NOT EXISTS idx_dep_edges_tgt ON dependency_edges(target_module_id)",
		"CREATE INDEX IF NOT EXISTS idx_metrics_owner ON metrics(owner_type, owner_id)",
	};
	bool ok = true;
	for (auto *sql : indexes) {
		if (!exec(sql)) {
			fprintf(stderr,
				"WARN: createIndexesAfterBulkLoad: %s\n",
				error_.c_str());
			ok = false;
		}
	}
	return ok;
}

// ─── Project Readiness ───────────────────────────────────────────

// ─── Index Progress (global, thread-safe) ────────────────────────
static std::mutex g_progress_mutex;
static store::IndexProgress g_index_progress;

void setIndexProgress(const IndexProgress &p)
{
	std::lock_guard<std::mutex> lock(g_progress_mutex);
	g_index_progress = p;
}

IndexProgress getIndexProgress()
{
	std::lock_guard<std::mutex> lock(g_progress_mutex);
	return g_index_progress;
}

std::string getIndexProgressJson(uint64_t project_id)
{
	auto p = getIndexProgress();
	char buf[512];
	int n = snprintf(buf, sizeof(buf),
			 "{\"project_id\":%llu,\"total_files\":%d,"
			 "\"current_file\":%d,\"phase\":%d,"
			 "\"percent\":%d,\"current_file_path\":\"%s\","
			 "\"error\":\"%s\"}",
			 (unsigned long long)p.project_id, p.total_files,
			 p.current_file, p.phase, p.percent,
			 p.current_file_path.c_str(), p.error.c_str());
	// Clamp the returned length to the bytes actually written into buf.
	// snprintf returns the would-be length (>= sizeof(buf)) on
	// truncation, or a negative value on error; without clamping,
	// std::string(buf, n) would read past the buffer.
	if (n < 0)
		return "{}";
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;
	return std::string(buf, n);
}

void GraphStore::setProjectReadiness(uint64_t project_id, const char *field,
				     int value)
{
	// Whitelist allowed field names to prevent SQL injection
	static const std::unordered_set<std::string> allowed_fields = {
		"fast_ready", "normal_ready", "deep_ready", "fts_ready",
		"vector_ready"
	};
	if (!field || allowed_fields.find(field) == allowed_fields.end())
		return;

	// Ensure the readiness row exists
	exec(std::string(
		     "INSERT OR IGNORE INTO project_readiness (project_id) VALUES (" +
		     std::to_string(project_id) + ")")
		     .c_str());
	exec(std::string("UPDATE project_readiness SET " + std::string(field) +
			 "=" + std::to_string(value) +
			 " WHERE project_id=" + std::to_string(project_id))
		     .c_str());
}

int GraphStore::getProjectReadiness(uint64_t project_id, const char *field)
{
	// Whitelist allowed field names to prevent SQL injection
	static const std::unordered_set<std::string> allowed_fields = {
		"fast_ready", "normal_ready", "deep_ready", "fts_ready",
		"vector_ready"
	};
	if (!field || allowed_fields.find(field) == allowed_fields.end())
		return 0;

	sqlite3_stmt *stmt = nullptr;
	std::string sql = "SELECT " + std::string(field) +
			  " FROM project_readiness WHERE project_id=" +
			  std::to_string(project_id);
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return 0;
	int val = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		val = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return val;
}

std::string GraphStore::getProjectReadinessJson(uint64_t project_id)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "SELECT fast_ready, normal_ready, deep_ready, "
			  "fts_ready, vector_ready "
			  "FROM project_readiness WHERE project_id=?";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string json;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		json = "\"fast_ready\":" +
		       std::to_string(sqlite3_column_int(stmt, 0)) +
		       ",\"normal_ready\":" +
		       std::to_string(sqlite3_column_int(stmt, 1)) +
		       ",\"deep_ready\":" +
		       std::to_string(sqlite3_column_int(stmt, 2)) +
		       ",\"fts_ready\":" +
		       std::to_string(sqlite3_column_int(stmt, 3)) +
		       ",\"vector_ready\":" +
		       std::to_string(sqlite3_column_int(stmt, 4));
	}
	sqlite3_finalize(stmt);
	return json;
}

// ─── Shared Artifact ─────────────────────────────────────────────
// Uses fork() + execvp() for zstd to avoid command injection via system().
// Validates paths reject single-quote chars to prevent SQL injection in VACUUM/ATTACH.

static bool pathHasMeta(const char *p)
{
	for (const char *c = p; *c; c++)
		if (*c == '\'' || *c == ';' || *c == '`' || *c == '$' ||
		    *c == '|' || *c == '&' || *c == '(' || *c == ')' ||
		    *c == '\\' || *c == '\n')
			return true;
	return false;
}

std::string GraphStore::exportArtifact(uint64_t project_id,
				       const char *output_path)
{
	if (!output_path || !*output_path || pathHasMeta(output_path))
		return "{\"ok\":false,\"error\":\"invalid output_path\"}";

	std::string tmp_path = std::string(output_path) + ".tmp";
	// VACUUM INTO — escape single quote by doubling
	std::string escaped_tmp = tmp_path;
	for (size_t p = 0; (p = escaped_tmp.find('\'', p)) != std::string::npos;
	     p += 2)
		escaped_tmp.insert(p, 1, '\'');
	std::string sql = "VACUUM INTO '" + escaped_tmp + "'";
	if (!exec(sql.c_str()))
		return "{\"ok\":false,\"error\":\"VACUUM INTO failed: " +
		       error_ + "\"}";

	uint64_t raw_size = 0;
	struct stat st;
	if (stat(tmp_path.c_str(), &st) == 0)
		raw_size = static_cast<uint64_t>(st.st_size);

	// fork + execvp for zstd compression — no shell involved
	pid_t pid = fork();
	int status = 0;
	if (pid == 0) {
		// Child process
		execlp("zstd", "zstd", "-9", "-f", "-q", tmp_path.c_str(), "-o",
		       output_path, nullptr);
		_exit(1); // exec failed
	} else if (pid > 0) {
		waitpid(pid, &status, 0);
	}
	std::remove(tmp_path.c_str());
	if (pid <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return "{\"ok\":false,\"error\":\"zstd compression failed\"}";

	uint64_t comp_size = 0;
	if (stat(output_path, &st) == 0)
		comp_size = static_cast<uint64_t>(st.st_size);
	return "{\"ok\":true,\"project_id\":" + std::to_string(project_id) +
	       ",\"size_bytes\":" + std::to_string(raw_size) +
	       ",\"compressed_bytes\":" + std::to_string(comp_size) + "}";
}

std::string GraphStore::importArtifact(uint64_t project_id,
				       const char *artifact_path)
{
	if (!artifact_path || !*artifact_path || pathHasMeta(artifact_path))
		return "{\"ok\":false,\"error\":\"invalid artifact_path\"}";

	std::string tmp_path = std::string(artifact_path) + ".decompressed";

	// fork + execvp for zstd decompression
	pid_t pid = fork();
	if (pid == 0) {
		execlp("zstd", "zstd", "-d", "-f", "-q", artifact_path, "-o",
		       tmp_path.c_str(), nullptr);
		_exit(1);
	}
	int dec_ok = 0;
	if (pid > 0) {
		int status;
		waitpid(pid, &status, 0);
		dec_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	if (!dec_ok) {
		std::remove(tmp_path.c_str());
		return "{\"ok\":false,\"error\":\"zstd decompression failed\"}";
	}

	// Escape tmp_path for SQL
	std::string escaped_tmp = tmp_path;
	for (size_t p = 0; (p = escaped_tmp.find('\'', p)) != std::string::npos;
	     p += 2)
		escaped_tmp.insert(p, 1, '\'');

	if (!exec(("ATTACH DATABASE '" + escaped_tmp + "' AS artifact")
			  .c_str())) {
		std::remove(tmp_path.c_str());
		return "{\"ok\":false,\"error\":\"attach artifact failed\"}";
	}

	// Transactional import scoped to project_id
	std::string pid_str = std::to_string(project_id);
	beginTransaction();
	bool ok = true;
	ok &= exec(
		("INSERT OR IGNORE INTO semantic_records SELECT * FROM artifact.semantic_records WHERE project_id=" +
		 pid_str)
			.c_str());
	ok &= exec(
		("INSERT OR IGNORE INTO graph_nodes SELECT * FROM artifact.graph_nodes WHERE project_id=" +
		 pid_str)
			.c_str());
	ok &= exec(
		("INSERT OR IGNORE INTO graph_edges SELECT * FROM artifact.graph_edges WHERE project_id=" +
		 pid_str)
			.c_str());
	if (!ok) {
		rollbackTransaction();
		exec("DETACH DATABASE artifact");
		std::remove(tmp_path.c_str());
		return "{\"ok\":false,\"error\":\"artifact import failed: " +
		       error_ + "\"}";
	}
	commitTransaction();
	exec("DETACH DATABASE artifact");
	std::remove(tmp_path.c_str());
	return "{\"ok\":true,\"project_id\":" + pid_str + "}";
}

// ─── Utility ───────────────────────────────────────────────────

bool GraphStore::exec(const char *sql)
{
	char *err = nullptr;
	int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		error_ = err ? err : "unknown error";
		sqlite3_free(err);
		return false;
	}
	return true;
}

void GraphStore::explainQueryPlan(const char *sql, const char *label)
{
	if (!sql || !*sql)
		return;
	std::string explain = std::string("EXPLAIN QUERY PLAN ") + sql;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, explain.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return;
	fprintf(stderr, "--- QPLAN %s ---\n", label ? label : "");
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int id = sqlite3_column_int(stmt, 0);
		int parent = sqlite3_column_int(stmt, 1);
		const char *detail = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		fprintf(stderr, "  id=%d parent=%d %s\n", id, parent,
			detail ? detail : "");
	}
	sqlite3_finalize(stmt);
}

// ─── Project ───────────────────────────────────────────────────

uint64_t GraphStore::createProject(const char *root_path, const char *name)
{
	// Try INSERT; if root_path already exists (UNIQUE constraint),
	// query the existing project ID instead.
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "INSERT OR IGNORE INTO projects (root_path, name) "
			  "VALUES (?, ?)";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_text(stmt, 1, root_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
	if (id == 0) {
		// Row already exists — query the existing ID
		id = getProjectId(root_path);
	}
	return id;
}

uint64_t GraphStore::getProjectId(const char *root_path)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "SELECT id FROM projects WHERE root_path = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_text(stmt, 1, root_path, -1, SQLITE_TRANSIENT);
	uint64_t id = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return id;
}

uint64_t GraphStore::getLatestProjectId()
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "SELECT id FROM projects ORDER BY id DESC LIMIT 1";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	uint64_t id = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return id;
}

// ─── File ──────────────────────────────────────────────────────

uint64_t GraphStore::upsertFile(uint64_t project_id, const char *path,
				const char *language, const char *content_hash)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT OR REPLACE INTO files (project_id, path, language, "
		"content_hash, last_parsed_at) "
		"VALUES (?, ?, ?, ?, datetime('now'))";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, language, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, content_hash, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	// Return file ID
	sql = "SELECT id FROM files WHERE project_id = ? AND path = ?";
	sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
	uint64_t file_id = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		file_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
	}
	sqlite3_finalize(stmt);
	return file_id;
}

} // namespace store
