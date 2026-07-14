#include "store.h"
#include "platform_win.h"

#include "posix_compat.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
static constexpr int kCacheSizePages = -64000;

// 64 MB cache
static constexpr int kPageSize =
	65536; // 64 KB pages (matches codebase-memory-mcp)

// Pattern adapted from codebase-memory-mcp src/store/store.c:cbm_store_resolve_mmap_size
// Resolves the SQLite mmap_size PRAGMA value from the CODESCOPE_MMAP_SIZE
// environment variable. Allows runtime tuning of memory-mapped I/O without
// recompiling — useful for very large projects that benefit from a bigger
// mmap window, or for constraining memory on small machines.
//
// Semantics:
//   - Unset or empty env var: return kDefaultMmapSize (256 MB)
//   - Non-numeric value: return kDefaultMmapSize, log a warning to stderr
//   - Negative value: return 0 (disables mmap, reverts to read()/pread())
//   - Valid non-negative integer: return the parsed value
static int64_t resolveMmapSize()
{
	static constexpr int64_t kDefaultMmapSize = 268435456; // 256 MB
	const char *env = std::getenv("CODESCOPE_MMAP_SIZE");
	if (!env || !*env)
		return kDefaultMmapSize;

	// Parse with strtoll — handles optional leading sign + whitespace.
	// endptr verifies the ENTIRE string was consumed; trailing non-whitespace
	// is rejected as garbage (e.g. "123abc" → default, not 123).
	errno = 0;
	char *endptr = nullptr;
	long long parsed = std::strtoll(env, &endptr, 10);
	if (endptr == env) {
		// No digits consumed — pure garbage
		fprintf(stderr,
			"WARN: CODESCOPE_MMAP_SIZE=\"%s\" is not numeric, "
			"using default %lld [module=store, "
			"method=resolveMmapSize]\n",
			env, static_cast<long long>(kDefaultMmapSize));
		return kDefaultMmapSize;
	}
	if (errno == ERANGE) {
		// Parsed value overflows long long — treat as invalid rather
		// than silently clamping to LLONG_MAX (~9.2 EB), which would
		// surprise users expecting a bounded mmap window.
		fprintf(stderr,
			"WARN: CODESCOPE_MMAP_SIZE=\"%s\" overflows, "
			"using default %lld [module=store, "
			"method=resolveMmapSize]\n",
			env, static_cast<long long>(kDefaultMmapSize));
		return kDefaultMmapSize;
	}
	// Allow trailing whitespace but reject other trailing characters
	while (endptr && *endptr != '\0') {
		if (*endptr != ' ' && *endptr != '\t') {
			fprintf(stderr,
				"WARN: CODESCOPE_MMAP_SIZE=\"%s\" has trailing "
				"garbage, using default %lld [module=store, "
				"method=resolveMmapSize]\n",
				env, static_cast<long long>(kDefaultMmapSize));
			return kDefaultMmapSize;
		}
		++endptr;
	}
	if (parsed < 0)
		return 0; // negative disables mmap entirely
	return static_cast<int64_t>(parsed);
}

// ── Query deadline (progress handler) ───────────────────────────
//
// Global atomic deadline in epoch milliseconds. 0 means "no limit" — the
// progress handler returns 0 (continue) immediately. When armed by
// setQueryDeadline(), the progress handler compares the current epoch ms
// against this value on every kProgressHandlerStepInterval VM steps; if
// exceeded, it returns 1 to abort the running query (sqlite3_step then
// returns SQLITE_INTERRUPT).
//
// This is global (not per-connection) because sqlite3_progress_handler is
// per-connection but the deadline semantics are per-search-call. The RAII
// guard ensures the deadline is cleared on every exit path, so the window
// of interference between concurrent search calls is small and bounded by
// the search duration. Non-search queries (buildGraph, inserts, etc.) are
// never affected because the deadline is 0 outside of a guard scope.
static std::atomic<int64_t> g_query_deadline_ms{ 0 };

/** Return the current epoch time in milliseconds. */
static int64_t currentEpochMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

/** Progress handler registered with sqlite3_progress_handler.
 *  Returns 1 (abort) when the armed deadline has passed, 0 otherwise.
 *  When g_query_deadline_ms is 0 the handler is a no-op. */
static int progressHandler(void * /*unused*/)
{
	int64_t deadline = g_query_deadline_ms.load(std::memory_order_relaxed);
	if (deadline == 0)
		return 0;
	return currentEpochMs() > deadline ? 1 : 0;
}

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
	exec(("PRAGMA mmap_size=" + std::to_string(resolveMmapSize())).c_str());

	// Register the query-abort progress handler. The handler is invoked
	// every kProgressHandlerStepInterval VM steps; it returns 1 (abort) only
	// when a deadline has been armed via setQueryDeadline(). Outside of a
	// QueryDeadlineGuard scope the deadline is 0, so the handler is a
	// no-op and imposes no measurable overhead on bulk inserts/buildGraph.
	sqlite3_progress_handler(db_, kProgressHandlerStepInterval,
				 &progressHandler, nullptr);

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

// ── Query deadline (progress handler) implementation ───────────

void GraphStore::setQueryDeadline(int timeout_ms)
{
	if (timeout_ms <= 0) {
		g_query_deadline_ms.store(0, std::memory_order_relaxed);
		return;
	}
	int64_t deadline = currentEpochMs() + timeout_ms;
	g_query_deadline_ms.store(deadline, std::memory_order_relaxed);
}

void GraphStore::clearQueryDeadline()
{
	g_query_deadline_ms.store(0, std::memory_order_relaxed);
}

GraphStore::QueryDeadlineGuard::QueryDeadlineGuard(GraphStore *store,
						   int timeout_ms)
	: store_(store)
{
	if (store_)
		store_->setQueryDeadline(timeout_ms);
}

GraphStore::QueryDeadlineGuard::~QueryDeadlineGuard()
{
	if (store_)
		store_->clearQueryDeadline();
}

// Schema DDL (createSchema, createIndexesAfterBulkLoad) lives in
// store_schema.cpp to keep this file under the 1000-line limit.

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
		"fast_ready", "normal_ready", "deep_ready",
		"fts_ready",  "vector_ready", "knowledge_ready"
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
		"fast_ready", "normal_ready", "deep_ready",
		"fts_ready",  "vector_ready", "knowledge_ready"
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
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
		fprintf(stderr,
			"createProject step failed (rc=%d): %s "
			"[module=store, method=createProject]\n",
			rc, sqlite3_errmsg(db_));
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
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE)
		fprintf(stderr,
			"upsertFile step failed (rc=%d): %s "
			"[module=store, method=upsertFile]\n",
			rc, sqlite3_errmsg(db_));
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
