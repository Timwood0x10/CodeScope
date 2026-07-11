#include "store.h"
#include "platform_win.h"

#include "posix_compat.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
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
static constexpr int kPageSize =
	65536; // 64 KB pages (matches codebase-memory-mcp)

GraphStore::~GraphStore()
{
	close();
}

bool GraphStore::open(const char *db_path)
{
	// Enable SQLite serialized threading mode so the same handle can be
	// used from multiple threads safely (worker pool in engine_index.cpp
	// + MCP server main loop both access g_store concurrently).
	// This MUST be called before any sqlite3_open() call.
	// If already initialized (SQLITE_MISUSE), the default is likely
	// serialized already (most builds have SQLITE_THREADSAFE=1).
	int config_rc = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
	if (config_rc == SQLITE_OK) {
		fprintf(stderr, "store: SQLite serialized threading enabled\n");
	} else if (config_rc == SQLITE_MISUSE) {
		fprintf(stderr,
			"store: SQLite already initialized, using existing threading mode\n");
	} else {
		fprintf(stderr,
			"store: sqlite3_config(SERIALIZED) returned %d\n",
			config_rc);
	}

	int rc = sqlite3_open(db_path, &db_);
	if (rc != SQLITE_OK) {
		error_ = sqlite3_errmsg(db_);
		return false;
	}
	db_path_ = db_path;

	// page_size MUST be set before any tables are created. Larger pages
	// (64 KB vs default 4 KB) reduce I/O ops and improve B-tree fanout —
	// critical for large projects with millions of rows. Only takes effect
	// on new databases; existing DBs keep their original page size.
	exec(("PRAGMA page_size=" + std::to_string(kPageSize)).c_str());

	// Performance PRAGMAs: WAL mode + MEMORY temp + synchronous OFF.
	// WARNING: synchronous=OFF means fsync is never called — a power failure
	// or crash may corrupt the database. Acceptable for CodeScope since the
	// DB is a cache that can be rebuilt by re-indexing. Use synchronous=NORMAL
	// if data safety is more important than write performance.
	if (!exec("PRAGMA journal_mode=WAL"))
		fprintf(stderr,
			"WARN: PRAGMA journal_mode=WAL failed: %s [module=store, method=open]\n",
			error_.c_str());
	// NORMAL locking mode: release locks between transactions so that
	// worker connections (for parallel enhance) can write concurrently.
	// EXCLUSIVE was used during development but blocks concurrent WAL writers.
	if (!exec("PRAGMA locking_mode=NORMAL"))
		fprintf(stderr,
			"WARN: PRAGMA locking_mode=NORMAL failed [module=store, method=open]\n");
	if (!exec("PRAGMA synchronous=OFF"))
		fprintf(stderr,
			"WARN: PRAGMA synchronous=OFF failed [module=store, method=open]\n");
	if (!exec("PRAGMA temp_store=MEMORY"))
		fprintf(stderr,
			"WARN: PRAGMA temp_store=MEMORY failed [module=store, method=open]\n");
	// Busy timeout: under concurrent access (worker pool threads + main loop),
	// WAL writers may conflict. Without a timeout, SQLite immediately returns
	// SQLITE_BUSY. 5000ms gives contenders time to finish their transaction.
	if (!exec("PRAGMA busy_timeout=5000"))
		fprintf(stderr,
			"WARN: PRAGMA busy_timeout=5000 failed [module=store, method=open]\n");
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

		// Finalize all dynamically cached statements
		clearStmtCache();

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
            is_stub INTEGER DEFAULT 0,
            callgraph_ready INTEGER DEFAULT 0,
            metrics_ready INTEGER DEFAULT 0,
            embedding_ready INTEGER DEFAULT 0,
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

        CREATE TABLE IF NOT EXISTS entity (
            id INTEGER PRIMARY KEY,
            project_id INTEGER NOT NULL,
            kind INTEGER NOT NULL,
            name TEXT NOT NULL,
            qualified_name TEXT DEFAULT '',
            file_path TEXT NOT NULL,
            language TEXT NOT NULL,
            start_row INTEGER NOT NULL,
            start_col INTEGER NOT NULL,
            end_row INTEGER NOT NULL,
            end_col INTEGER NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        CREATE TABLE IF NOT EXISTS relation (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            source_id INTEGER NOT NULL,
            target_id INTEGER NOT NULL,
            type INTEGER NOT NULL,
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (source_id) REFERENCES entity(id),
            FOREIGN KEY (target_id) REFERENCES entity(id)
        );

        CREATE INDEX IF NOT EXISTS idx_files_project ON files(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_project ON graph_nodes(project_id);
        CREATE INDEX IF NOT EXISTS idx_graph_nodes_name ON graph_nodes(project_id, name);
        -- Composite index for _r2n JOIN during buildGraph:
        -- graph_nodes JOIN semantic_records ON (project_id, file_path, start_row, node_type=kind)
        CREATE INDEX IF NOT EXISTS idx_gn_file_row_type ON graph_nodes(project_id, file_path, start_row, node_type);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_src ON graph_edges(source_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_tgt ON graph_edges(target_node_id);
        CREATE INDEX IF NOT EXISTS idx_graph_edges_project ON graph_edges(project_id);
        -- Composite indexes for caller/callee queries: edge_type + node_id
        -- getCallers: WHERE edge_type=1 AND target_node_id IN (SELECT id FROM graph_nodes WHERE name=?)
        -- getCallees: WHERE edge_type=1 AND source_node_id IN (SELECT id FROM graph_nodes WHERE name=?)
        CREATE INDEX IF NOT EXISTS idx_ge_callers ON graph_edges(edge_type, target_node_id);
        CREATE INDEX IF NOT EXISTS idx_ge_callees ON graph_edges(edge_type, source_node_id);
        -- Deduplicate existing edges before creating unique constraint
        DELETE FROM graph_edges WHERE id NOT IN (
          SELECT MIN(id) FROM graph_edges
          GROUP BY source_node_id, target_node_id, edge_type
        );
        CREATE UNIQUE INDEX IF NOT EXISTS idx_ge_unique_edge
          ON graph_edges(source_node_id, target_node_id, edge_type);

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
            ref_original_id INTEGER DEFAULT 0,
            arity INTEGER DEFAULT 0,
            is_static INTEGER DEFAULT 0,
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
        -- Language added for P3 cross-file matching: sr.language = callee.language
        CREATE INDEX IF NOT EXISTS idx_sr_kind_name ON semantic_records(project_id, kind, name, language);
        -- Index for _r2n file filter: WHERE kind IN (...) AND file_path IN (SELECT ...)
        CREATE INDEX IF NOT EXISTS idx_sr_kind_fp ON semantic_records(project_id, kind, file_path);

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

        -- Phase 2: adjacency (CSR BLOB). Aggregated from graph_edges(edge_type=1)
        -- after buildGraph. Each row packs all callee node IDs for a caller into a
        -- contiguous u32 BLOB. Queries are O(1) B-tree lookup + pointer arithmetic.
        CREATE TABLE IF NOT EXISTS adjacency (
            src_id INTEGER PRIMARY KEY,    -- graph_nodes.id (caller)
            project_id INTEGER NOT NULL,
            tgt_blob BLOB                  -- packed u32[] of callee node IDs
        );

        -- Phase 2: reverse adjacency (CSR BLOB). Mirror of adjacency for
        -- caller lookups: each row packs all caller node IDs for a callee.
        -- Enables O(1) getCallerIds() instead of O(n) full-scan.
        CREATE TABLE IF NOT EXISTS adjacency_rev (
            tgt_id INTEGER PRIMARY KEY,    -- graph_nodes.id (callee)
            project_id INTEGER NOT NULL,
            src_blob BLOB                  -- packed u32[] of caller node IDs
        );

        -- ============================================================
        -- v0.3: Knowledge + Evidence Layer
        -- capability/contract = Knowledge, claim/evidence/evidence_fact/
        -- finding = Evidence. All tables use CREATE TABLE IF NOT EXISTS so
        -- no migration is needed for pre-existing databases.
        -- ============================================================

        -- capability: a feature the project claims to provide.
        CREATE TABLE IF NOT EXISTS capability (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            summary TEXT DEFAULT '',
            source_kind TEXT NOT NULL,      -- 'readme' / 'doc' / 'heuristic'
            source_ref TEXT DEFAULT '',     -- file path or rule name
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- contract: an architectural / quality promise (e.g. "ThreadSafe").
        CREATE TABLE IF NOT EXISTS contract (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            origin TEXT NOT NULL,           -- 'readme' / 'comment' / 'architecture'
            claim_text TEXT DEFAULT '',
            source_file TEXT DEFAULT '',
            source_line INTEGER DEFAULT 0,
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- claim: the unified intermediate representation fed to verifiers.
        CREATE TABLE IF NOT EXISTS claim (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            claim_type INTEGER NOT NULL,    -- verify::ClaimType enum value
            subject TEXT NOT NULL,
            predicate TEXT NOT NULL,
            object TEXT DEFAULT '',
            scope TEXT DEFAULT 'repository',
            source_kind TEXT NOT NULL,     -- 'readme' / 'ai_summary' / 'pr' / 'manual'
            source_ref TEXT DEFAULT '',
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id)
        );

        -- evidence: outcome of a verifier run on a single claim.
        CREATE TABLE IF NOT EXISTS evidence (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            claim_id INTEGER NOT NULL,
            verdict INTEGER NOT NULL,      -- 0=SUPPORTED, 1=CONTRADICTED, 2=UNKNOWN
            confidence REAL NOT NULL,      -- 0.0 - 1.0
            verifier_name TEXT NOT NULL,
            detail TEXT DEFAULT '',
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (claim_id) REFERENCES claim(id)
        );

        -- evidence_fact: links evidence back to entity/relation rows.
        CREATE TABLE IF NOT EXISTS evidence_fact (
            evidence_id INTEGER NOT NULL,
            fact_kind INTEGER NOT NULL,    -- 0=entity, 1=relation, 2=document
            fact_ref INTEGER NOT NULL,     -- entity.id / relation.id / doc rowid
            detail TEXT DEFAULT '',
            PRIMARY KEY (evidence_id, fact_kind, fact_ref),
            FOREIGN KEY (evidence_id) REFERENCES evidence(id)
        );

        -- finding: a human-facing issue derived from evidence (may be manual).
        CREATE TABLE IF NOT EXISTS finding (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL,
            rule TEXT NOT NULL,            -- e.g. "DeadCapability"
            severity INTEGER NOT NULL DEFAULT 1, -- 0=info, 1=warning, 2=error
            claim_id INTEGER,             -- nullable: manual findings need no claim
            description TEXT NOT NULL,
            confidence REAL DEFAULT 0.0,
            created_at TEXT DEFAULT (datetime('now')),
            FOREIGN KEY (project_id) REFERENCES projects(id),
            FOREIGN KEY (claim_id) REFERENCES claim(id)
        );

        CREATE INDEX IF NOT EXISTS idx_capability_project ON capability(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_contract_project ON contract(project_id, name);
        CREATE INDEX IF NOT EXISTS idx_claim_project ON claim(project_id, claim_type);
        CREATE INDEX IF NOT EXISTS idx_evidence_claim ON evidence(claim_id);
        CREATE INDEX IF NOT EXISTS idx_finding_project ON finding(project_id, rule);

        )SQL";

	// Execute main schema
	bool ok = exec(schema);

	// ── Schema migrations for pre-existing databases ───────────────
	// CREATE TABLE IF NOT EXISTS skips when the table already exists, so columns
	// added in later versions must be patched in here. SQLite has no
	// "ADD COLUMN IF NOT EXISTS", so we probe PRAGMA table_info first.

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
		"CREATE INDEX IF NOT EXISTS idx_graph_nodes_lang ON graph_nodes(project_id, language)",
	};
	bool ok = true;
	for (auto *sql : indexes) {
		if (!exec(sql)) {
			fprintf(stderr,
				"WARN: createIndexesAfterBulkLoad: %s [module=store, method=createIndexesAfterBulkLoad]\n",
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

// Escape a string for safe inclusion in a JSON string literal.
// Handles ", \, and control characters per RFC 8259.
static std::string jsonEscapeForProgress(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x",
					 static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out;
}

std::string getIndexProgressJson(uint64_t project_id)
{
	auto p = getIndexProgress();
	std::ostringstream oss;
	oss << "{\"project_id\":" << (unsigned long long)p.project_id
	    << ",\"total_files\":" << p.total_files
	    << ",\"current_file\":" << p.current_file
	    << ",\"phase\":" << p.phase << ",\"percent\":" << p.percent
	    << ",\"current_file_path\":\""
	    << jsonEscapeForProgress(p.current_file_path) << "\""
	    << ",\"error\":\"" << jsonEscapeForProgress(p.error) << "\"}";
	return oss.str();
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
	std::error_code ec;
	raw_size = std::filesystem::file_size(tmp_path, ec);

	// zstd compression — no shell involved
	bool zstd_ok = false;
#ifndef _WIN32
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
	zstd_ok = (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#else
	// Windows: _spawnlp with _P_WAIT blocks until the child exits and
	// returns its exit code directly — no fork/waitpid pair needed.
	int exit_code = _spawnlp(_P_WAIT, "zstd", "zstd", "-9", "-f", "-q",
				 tmp_path.c_str(), "-o", output_path, nullptr);
	zstd_ok = (exit_code == 0);
#endif
	std::remove(tmp_path.c_str());
	if (!zstd_ok)
		return "{\"ok\":false,\"error\":\"zstd compression failed\"}";

	uint64_t comp_size = 0;
	comp_size = std::filesystem::file_size(output_path, ec);
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

	// zstd decompression — no shell involved
	bool dec_ok = false;
#ifndef _WIN32
	pid_t pid = fork();
	if (pid == 0) {
		execlp("zstd", "zstd", "-d", "-f", "-q", artifact_path, "-o",
		       tmp_path.c_str(), nullptr);
		_exit(1);
	}
	if (pid > 0) {
		int status;
		waitpid(pid, &status, 0);
		dec_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
#else
	// Windows: _spawnlp with _P_WAIT returns the exit code directly.
	int exit_code = _spawnlp(_P_WAIT, "zstd", "zstd", "-d", "-f", "-q",
				 artifact_path, "-o", tmp_path.c_str(),
				 nullptr);
	dec_ok = (exit_code == 0);
#endif
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
