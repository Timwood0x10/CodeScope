#ifndef STORE_H
#define STORE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../graph/graph_types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace ir
{
struct Record;
} // namespace ir

// Forward declarations for the Knowledge/Evidence types defined in
// verify/claim.h. Kept as forward decls to avoid pulling claim.h into
// every store consumer; the implementations live in store_knowledge.cpp.
namespace verify
{
struct Claim;
enum class Verdict : uint8_t;
} // namespace verify

namespace store
{

// ─── Streaming Pipeline Data Structures ──────────────────────────
//
// FileResult bundle: passed from parse worker to single writer thread.
// MetricsRow, StubRow, LocalCall: pre-computed in parse worker.

/**
 * Pre-computed metrics for a single symbol.
 * Populated during the parse phase (not in enhance) to avoid
 * a second parse/translate pass.
 *
 * Fields correspond to the `metrics` table schema:
 *   (project_id, owner_type, owner_id, cyclomatic, nesting_depth,
 *    cognitive, lines, param_count, call_count, branch_count, loop_count)
 * project_id is filled by the writer, not stored in the row.
 *
 * name/line/col are used to JOIN with the symbols table at resolve time
 * (symbol IDs are not known until after buildGraph + populateSymbolsFromGraph).
 */
struct MetricRow {
	std::string name; // symbol name, for symbol JOIN at resolve time
	int line = 0; // symbol start line, for symbol JOIN at resolve time
	int col = 0; // symbol start column, for symbol JOIN at resolve time
	int cyclomatic = 0;
	int nesting_depth = 0;
	int cognitive = 0;
	int lines = 0;
	int param_count = 0;
	int call_count = 0;
	int branch_count = 0;
	int loop_count = 0;
	bool is_stub = false; // true = function body has no real statements
};

/**
 * FileResult: 单个文件 parse 的全部产出。
 *
 * Worker 线程 parse 完一个文件后产出 FileResult，推入 BoundedQueue，
 * 由单 writer 线程批量落库。Worker 推完即释放内存（AST/IR/source buffer）。
 *
 * 包含 semantic_records、预计算 metrics、stub 标志、同文件 call edges。
 * 不包含 graph_nodes——那是 buildGraph 阶段由 SQL 生成的。
 */
struct FileResult {
	std::string file_path;
	std::string language;
	std::vector<ir::Record> records;
	std::vector<MetricRow> metrics;
	int64_t mtime = 0;
	int64_t fsize = 0;
};

class GraphStore {
    public:
	GraphStore() = default;
	~GraphStore();

	GraphStore(const GraphStore &) = delete;
	GraphStore &operator=(const GraphStore &) = delete;

	bool open(const char *db_path);

	/** Get the database file path (for opening additional connections). */
	const std::string &dbPath() const
	{
		return db_path_;
	}
	void close();

	// ── Project ────────────────────────────────────────────────

	uint64_t createProject(const char *root_path, const char *name);
	uint64_t getProjectId(const char *root_path);
	uint64_t getLatestProjectId();

	// ── File ───────────────────────────────────────────────────

	uint64_t upsertFile(uint64_t project_id, const char *path,
			    const char *language, const char *content_hash);

	// ── Graph Nodes ────────────────────────────────────────────

	uint64_t insertGraphNode(uint64_t project_id,
				 const graph::GraphNode &node);

	/** Batch insert graph nodes — prepare once, bind/step/reset in a loop.
	 *  Up to ~10x faster than calling insertGraphNode per node. */
	void insertGraphNodes(uint64_t project_id,
			      const std::vector<graph::GraphNode> &nodes);

	bool deleteGraphNodesByFile(uint64_t project_id, const char *file_path);

	// ── Graph Edges ────────────────────────────────────────────

	uint64_t insertGraphEdge(uint64_t project_id,
				 const graph::GraphEdge &edge);

	/** Batch insert graph edges — prepare once, bind/step/reset in a loop. */
	void insertGraphEdges(uint64_t project_id,
			      const std::vector<graph::GraphEdge> &edges);

	bool deleteGraphEdgesByFile(uint64_t project_id, const char *file_path);

	// ── Entity/Relation (Phase 1.1) ─────────────────────────

	uint64_t insertEntity(uint64_t project_id,
			      const graph::GraphNode &node);
	void insertRelation(uint64_t project_id, uint64_t source_id,
			    uint64_t target_id, int type);

	// ── Semantic Records (DB-first pipeline) ───────────────────

	/**
	 * Batch insert semantic records into a persistent table.
	 * Records are stored flat (no graph) — graph is built on-demand
	 * via buildGraph(). This keeps parse-time memory O(1) per file.
	 *
	 * @param project_id  Project identifier.
	 * @param file_path   Source file path for all records.
	 * @param records     Flat vector of semantic records (parent_id links).
	 */
	void insertSemanticRecords(uint64_t project_id,
				   const std::string &file_path,
				   const std::vector<ir::Record> &records);

	/**
	 * Batch insert semantic records for MULTIPLE files in one call.
	 * Prepares the SQL statement ONCE and reuses it for all records
	 * across all files — significantly reducing per-file prepare/finalize
	 * overhead compared to calling insertSemanticRecords per file.
	 *
	 * @param project_id     Project identifier.
	 * @param file_records   Vector of (file_path, records) pairs.
	 */
	void insertSemanticRecordsBatch(
		uint64_t project_id,
		const std::vector<
			std::pair<std::string, std::vector<ir::Record> > >
			&file_records);

	/**
	 * Streaming pipeline: batch-insert a complete FileResult bundle.
	 *
	 * Writes semantic_records, metrics, symbol_status (stub), and local
	 * call_edges in a single transaction. Called by the single writer
	 * thread, NOT by parse workers (which only push to the queue).
	 *
	 * This replaces the old two-phase flow (index + re-parse enhance):
	 * metrics and stubs are pre-computed in the parse worker so the
	 * enhance phase no longer needs to re-parse or re-translate files.
	 *
	 * @param project_id  Project identifier.
	 * @param batch       Vector of FileResult bundles (one per file).
	 * @return true on success.
	 */
	bool insertFileResultBatch(uint64_t project_id,
				   const std::vector<FileResult> &batch);

	/**
	 * Resolve pre-computed metrics from the staging temp table into
	 * the metrics and symbol_status tables.
	 *
	 * Must be called AFTER buildGraph + populateSymbolsFromGraph, because
	 * metrics reference symbol IDs which are created during populate.
	 *
	 * Does a single SQL JOIN to resolve all staged metrics in O(n) time.
	 *
	 * @param project_id  Project identifier.
	 * @return true on success.
	 */
	bool resolveStagedMetrics(uint64_t project_id);

	/**
	 * Populate the symbols table from graph_nodes for a project.
	 * Called after buildGraph to create symbol entries with node_id
	 * back-references for cross-file edge copy.
	 *
	 * @param project_id  Project identifier.
	 * @return Number of symbols inserted.
	 */
	int64_t populateSymbolsFromGraph(uint64_t project_id);

	/**
	 * Build the knowledge graph from previously stored semantic records.
	 * Reads records from semantic_records table, runs GraphBuilder,
	 * writes graph_nodes and graph_edges. Idempotent — deletes and
	 * rebuilds if called again.
	 *
	 * @param project_id  Project to build graph for.
	 * @param build_calls If true (default), also builds call graph edges
	 *                    via cross-file name matching. If false, builds
	 *                    only symbol/containment graph (~2x faster).
	 * @param changed_files If non-null, only rebuild graph for these files
	 *                      (incremental mode). When null, rebuilds all.
	 * @return true on success.
	 */
	bool buildGraph(
		uint64_t project_id, bool build_calls = true,
		const std::unordered_set<std::string> *changed_files = nullptr);

	// ── CSR Adjacency (BLOB-packed call edges) ──────────────────

	/** Build adjacency (CSR BLOB) from graph_edges(edge_type=1).
	 *  Call after buildGraph. Drops and rebuilds adjacency. */
	bool buildCSR(uint64_t project_id);
	/** Get callee node IDs for a given source (caller) node via adjacency.
	 *  Returns empty vector on miss. O(1) B-tree lookup. */
	std::vector<uint64_t> getCalleeIds(uint64_t node_id);
	/** Get caller node IDs for a given target (callee) node via adjacency.
	 *  Returns empty vector on miss. O(1) B-tree lookup via adjacency_rev. */
	std::vector<uint64_t> getCallerIds(uint64_t node_id);

	// ── Transactions ───────────────────────────────────────────

	bool beginTransaction();
	bool commitTransaction();
	bool rollbackTransaction();

	// ── Search ─────────────────────────────────────────────────

	std::string searchCode(uint64_t project_id, const char *query,
			       int limit);

	// ── FTS index helpers ───────────────────────────────────────

	void insertIntoFTS(uint64_t node_id, uint64_t project_id,
			   const char *name, const char *qualified_name,
			   const char *file_path, const char *content,
			   int node_kind = -1);

	/**
	 * Bulk-build FTS index from graph_nodes for a project.
	 * Scans all graph_nodes with a non-empty name and inserts into
	 * code_fts + fts_node_map in one pass. Designed for deferred
	 * indexing after the main batch insert.
	 */
	void buildFTSFromGraph(uint64_t project_id);

	// ── Vector search (semantic) ─────────────────────────────────

	bool storeVector(uint64_t node_id, uint64_t project_id,
			 const void *vec_data, size_t vec_bytes);
	std::string searchSemantic(uint64_t project_id, const void *query_vec,
				   size_t vec_bytes, int limit);
	// deleteFTSByFile removed — FTS is built inline during buildGraph

	/**
	 * Bulk-build semantic vectors from graph_nodes for a project.
	 * Reads node names, converts to n-gram hash vectors, and stores
	 * in node_vectors. Designed for deferred indexing.
	 */
	void buildVectorsFromGraph(uint64_t project_id);

	// ── Complexity ───────────────────────────────────────────────

	bool setComplexity(uint64_t project_id, uint64_t graph_node_id,
			   uint64_t cyclomatic, uint64_t cognitive,
			   uint64_t nesting_depth, uint64_t decision_points);
	std::string getComplexityJson(uint64_t project_id,
				      uint64_t graph_node_id);

	// ── New Schema (Phase A): Modules ─────────────────────────

	/**
     * Insert or get a module (directory) node.
     * Returns the module id.
     */
	uint64_t insertModule(uint64_t project_id, uint64_t parent_id,
			      const char *name, const char *path,
			      const char *language);

	// ── New Schema (Phase A): Symbols ─────────────────────────

	/**
     * Insert a symbol (fast scan result).
     * Automatically creates a corresponding row in symbol_status.
     * Returns the symbol id.
     */
	uint64_t insertSymbol(uint64_t project_id, uint64_t module_id,
			      const char *kind, const char *name,
			      const char *signature, const char *visibility,
			      const char *language, const char *file_path,
			      int line, int column, int span_start,
			      int span_end);

	// ── New Schema (Phase A): Entry Points ────────────────────

	bool insertEntryPoint(uint64_t symbol_id, uint64_t project_id,
			      const char *kind);

	// ── New Schema (Phase A): Queries ─────────────────────────

	/** Get module tree as JSON. */
	std::string getModuleTreeJson(uint64_t project_id);

	/** Find symbols by name as JSON. */
	std::string findSymbolJson(uint64_t project_id, const char *name);

	// ── Phase B: Enhancement — Call Edges ─────────────────────

	uint64_t insertCallEdge(uint64_t project_id, uint64_t caller_symbol_id,
				uint64_t callee_symbol_id,
				const char *provenance, int line, int col);

	// ── Phase B: Enhancement — Embeddings ─────────────────────

	bool insertEmbedding(uint64_t symbol_id, const float *vector_data,
			     int dim);

	// ── Symbol Status (separate from symbols, keeps main table lean) ──

	/** Mark a symbol's callgraph and metrics as ready (embedding is separate
	 *  because it may fail when vec0 is unavailable). */
	bool markCallgraphAndMetricsReady(uint64_t symbol_id);
	/** Mark a symbol's embedding as ready. Only call after insertEmbedding
	 *  succeeded, so that failed embeddings can self-heal on rerun. */
	bool markEmbeddingReady(uint64_t symbol_id);
	bool setSymbolStub(uint64_t symbol_id, bool is_stub);

	/** Get files where symbols have a status flag = 0 (not ready). */
	std::vector<std::string> getUnreadyFiles(uint64_t project_id,
						 const char *ready_field);

	/** Get ratio of symbols with a status flag = 1 (0.0 - 1.0). */
	double getReadyRatio(uint64_t project_id);
	double getReadyRatio(uint64_t project_id, const char *ready_field);

	// ── Incremental Indexing ──────────────────────────────────

	bool isFileUnchanged(uint64_t project_id, const char *file_path,
			     int64_t mtime, int64_t size);
	void updateFileScanState(uint64_t project_id, const char *file_path,
				 int64_t mtime, int64_t size);
	void cleanupStaleFiles(uint64_t project_id,
			       const std::vector<std::string> &existing_files);

	// ── Path Tracing (BFS on call_edges) ──────────────────────

	/**
     * Trace the shortest call path between two functions using BFS on call_edges.
     * Returns JSON: {"path": [{"name":"...","file":"...","line":N}, ...]}
     * Requires callgraph_ready (enhancement must be run first).
     */
	std::string tracePathJson(uint64_t project_id, const char *from_name,
				  const char *to_name);

	/**
	 * Explore a function's callers/callees recursively as a JSON tree.
	 * Returns hierarchical JSON: {"name":"...","file":"...","line":N,"callers":[...],"callees":[...]}
	 * Each level nests up to `depth` levels (0 = just the function metadata).
	 * @param function_name Starting function.
	 * @param depth How many levels to recurse (max 5).
	 * @param direction "callers", "callees", or "both".
	 */
	std::string exploreFunctionJson(uint64_t project_id,
					const char *function_name, int depth,
					const char *direction);

	// ── Index Tasks (Tokio background task tracking) ──────────

	/** Create a new task record. Returns task id. */
	uint64_t createTask(uint64_t project_id, const char *task_type);

	/** Update task status and progress. */
	bool updateTask(uint64_t task_id, const char *status, int progress,
			const char *error);

	/** Get latest task for a project as JSON. */
	std::string getTaskStatusJson(uint64_t project_id);

	// ── Phase C: Unified Queries (adaptive) ───────────────────

	/**
	     * Unified search: auto-selects between FTS5 and semantic search
	     * based on embedding_ready ratio. Falls back to graph-based name
	     * matching if FTS is not ready.
	     */
	std::string searchUnifiedJson(uint64_t project_id, const char *query,
				      int limit);
	/**
	 * Graph-based search fallback: searches graph_nodes.name using LIKE.
	 * Used when FTS is not yet built (fts_ready=0).
	 */
	std::string searchGraphFallback(uint64_t project_id, const char *query,
					int limit);

	/**
     * Find callers from the new call_edges table (requires callgraph_ready).
     * Returns JSON array of caller symbols.
     */
	std::string findCallersJson(uint64_t project_id,
				    const char *symbol_name);

	/**
     * Find callees from the new call_edges table (requires callgraph_ready).
     * Returns JSON array of callee symbols.
     */
	std::string findCalleesJson(uint64_t project_id,
				    const char *symbol_name);

	/** Get entry points from the new entry_points table. */
	std::string getEntryPointsJson(uint64_t project_id);

	// ── On-demand call graph queries (from semantic_records, not pre-built) ──

	/**
	 * Create indexes after bulk data load.
	 * Call this once after all semantic_records and graph_nodes have been inserted.
	 */
	bool createIndexesAfterBulkLoad(uint64_t project_id);

	sqlite3 *handle() const
	{
		return db_;
	}

	const std::string &error() const
	{
		return error_;
	}

	/** Debug: run EXPLAIN QUERY PLAN and log output to stderr. */
	void explainQueryPlan(const char *sql, const char *label = nullptr);

	// ── Project Readiness ──────────────────────────────────────

	/** Set a readiness flag for a project. */
	void setProjectReadiness(uint64_t project_id, const char *field,
				 int value);

	/** Get a readiness flag for a project, or 0 if not set. */
	int getProjectReadiness(uint64_t project_id, const char *field);

	/** Get all readiness flags as JSON: "fast_ready":1,"fts_ready":0,... */
	std::string getProjectReadinessJson(uint64_t project_id);

	// ── Shared Artifact ─────────────────────────────────────────

	/**
	 * Export a compact DB artifact (VACUUM INTO + zstd).
	 * Returns JSON result string.
	 */
	std::string exportArtifact(uint64_t project_id,
				   const char *output_path);

	/**
	 * Import a previously exported artifact.
	 * Returns JSON result string.
	 */
	std::string importArtifact(uint64_t project_id,
				   const char *artifact_path);

	// ── Knowledge + Evidence Layer (v0.3) ───────────────────────
	// Persistence for capability/contract/claim/evidence/finding.
	// Implemented in store_knowledge.cpp. Inserts use the prepared
	// statement cache (see getCachedStmt) and SQLITE_STATIC for strings.

	/** Insert a capability row. Returns true on success. */
	bool insertCapability(uint64_t project_id, const std::string &name,
			      const std::string &summary,
			      const std::string &source_kind,
			      const std::string &source_ref);

	/** Insert a contract row. Returns true on success. */
	bool insertContract(uint64_t project_id, const std::string &name,
			    const std::string &origin,
			    const std::string &claim_text,
			    const std::string &source_file, int source_line);

	/** Insert a claim row derived from a verify::Claim.
	 *  @return new claim id, or -1 on error. */
	int64_t insertClaim(uint64_t project_id, const verify::Claim &claim);

	/** Insert an evidence row for a claim.
	 *  @return new evidence id, or -1 on error. */
	int64_t insertEvidence(int64_t claim_id, verify::Verdict verdict,
			       double confidence,
			       const std::string &verifier_name,
			       const std::string &detail);

	/** Link an evidence row to a fact (entity/relation/document). */
	bool insertEvidenceFact(int64_t evidence_id, int fact_kind,
				int64_t fact_ref, const std::string &detail);

	/** Insert a finding row. claim_id may be 0 for manual findings.
	 *  @return new finding id, or -1 on error. */
	int64_t insertFinding(uint64_t project_id, const std::string &rule,
			      int severity, int64_t claim_id,
			      const std::string &description,
			      double confidence);

	/** Delete all knowledge/evidence rows for a project.
	 *  Deletes in FK order: evidence_fact -> evidence -> finding ->
	 *  claim -> contract -> capability. Returns true on success. */
	bool clearProjectKnowledge(uint64_t project_id);

	/** List (id, name) pairs of capabilities for a project. */
	std::vector<std::pair<int64_t, std::string> >
	listCapabilities(uint64_t project_id);

	/** List (id, name) pairs of contracts for a project. */
	std::vector<std::pair<int64_t, std::string> >
	listContracts(uint64_t project_id);

    private:
	sqlite3 *db_ = nullptr;
	std::string error_;
	std::string db_path_;

	// Cached prepared statements (initialized in open(), finalized in close())
	sqlite3_stmt *stmt_fts_map_ = nullptr; // INSERT INTO fts_node_map
	sqlite3_stmt *stmt_fts_ = nullptr; // INSERT INTO code_fts
	sqlite3_stmt *stmt_vector_ = nullptr; // INSERT INTO node_vectors

	// Dynamic statement cache keyed by SQL text. Reused across Phase B writes.
	// The mutex only guards the cache map (insert/find/reset). The returned
	// sqlite3_stmt* is bind/stepped by the caller WITHOUT the lock — so all
	// GraphStore write paths must be serialized by the caller (single-threaded
	// enhance/index, or external locking). Do NOT call GraphStore methods
	// concurrently from multiple threads.
	// Soft cap at kStmtCacheMax; exceeding it means a bug (dynamic SQL at runtime).
	static constexpr size_t kStmtCacheMax = 32;
	std::unordered_map<std::string, sqlite3_stmt *> stmt_cache_;
	std::mutex stmt_cache_mutex_;
	// Returns a cached + reset prepared statement for `sql`. Prepares on first
	// use; caller must bind + step, and must NOT finalize (owned by cache).
	// Returns nullptr on prepare failure (error_ is set).
	sqlite3_stmt *getCachedStmt(const char *sql);
	// Release all cached prepared statements and clear the cache.
	// Safe to call at any point; subsequent getCachedStmt calls will
	// re-prepare. Already called by close().
	void clearStmtCache();

	bool exec(const char *sql);
	bool createSchema();

	// ── Internal: SQL-based call edge resolution ──────────────

	/**
	 * Build call graph edges (edge_type=1) using SQL JOINs instead of
	 * C++ hash maps. Replaces the in-memory caller_idx / callee_by_name /
	 * decl_idx / ir_edge_target maps with SQL temp tables + indexes.
	 *
	 * Memory: O(batch_size) instead of O(total_nodes). For 4M nodes,
	 * this reduces peak RSS from ~2-4GB to ~64MB (SQLite cache).
	 *
	 * Prerequisites: _r2n temp table must exist (created by buildGraph).
	 *
	 * @param project_id  Project to build call edges for.
	 * @return Number of call edges inserted, or -1 on error.
	 */
	int64_t buildCallEdgesSQL(uint64_t project_id);
};

// ── Index Progress (global, for client polling) ─────────────

/**
 * Index progress snapshot for a project.
 * Written during index_project and read by client polling.
 * Thread-safe via atomic store.
 */
struct IndexProgress {
	uint64_t project_id = 0;
	int total_files = 0;
	int current_file = 0;
	int phase =
		0; // 0:scanning 1:parsing 2:linking 3:building_graph 4:building_fts 5:done
	int percent = 0; // 0-100
	std::string current_file_path;
	std::string error;
};

/** Atomically set the current index progress. */
void setIndexProgress(const IndexProgress &p);

/** Atomically get the current index progress. */
IndexProgress getIndexProgress();

/** Get index progress as JSON for FFI. */
std::string getIndexProgressJson(uint64_t project_id);

} // namespace store

#endif // STORE_H
