#ifndef STORE_H
#define STORE_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "../graph/graph_types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace ir
{
struct Record;
} // namespace ir

namespace store
{

class GraphStore {
    public:
	GraphStore() = default;
	~GraphStore();

	GraphStore(const GraphStore &) = delete;
	GraphStore &operator=(const GraphStore &) = delete;

	bool open(const char *db_path);
	void close();

	// ── Project ────────────────────────────────────────────────

	uint64_t createProject(const char *root_path, const char *name);
	uint64_t getProjectId(const char *root_path);

	// ── File ───────────────────────────────────────────────────

	uint64_t upsertFile(uint64_t project_id, const char *path,
			    const char *language, const char *content_hash);

	// ── IR Nodes ───────────────────────────────────────────────

	uint64_t insertIRNode(uint64_t project_id, uint64_t file_id,
			      uint64_t parent_id, int kind, const char *name,
			      const char *qualified_name, uint32_t sr,
			      uint32_t sc, uint32_t er, uint32_t ec,
			      const char *language);

	bool insertIRSemanticEdge(uint64_t project_id, uint64_t source_id,
				  uint64_t target_id, int relation);

	bool deleteIRByFile(uint64_t project_id, uint64_t file_id);

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
	void deleteFTSByFile(uint64_t project_id, uint64_t file_id);

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

	uint64_t insertDependencyEdge(uint64_t project_id,
				      uint64_t source_module_id,
				      uint64_t target_module_id,
				      const char *external_name,
				      const char *kind);

	// ── Phase B: Enhancement — Metrics ────────────────────────

	bool insertMetric(uint64_t project_id, const char *owner_type,
			  uint64_t owner_id, int cyclomatic, int nesting_depth,
			  int cognitive, int lines, int param_count,
			  int call_count, int branch_count, int loop_count);

	// ── Phase B: Enhancement — Search Index ───────────────────

	void insertIntoSearchIndex(uint64_t symbol_id, uint64_t project_id,
				   const char *title, const char *summary,
				   const char *body);

	// ── Phase B: Enhancement — Embeddings ─────────────────────

	bool insertEmbedding(uint64_t symbol_id, const float *vector_data,
			     int dim);

	// ── Symbol Status (separate from symbols, keeps main table lean) ──

	/** Set a specific status flag (callgraph_ready/metrics_ready/embedding_ready)
     * to 1. */
	bool setSymbolReady(uint64_t symbol_id, const char *field);
	bool setSymbolStub(uint64_t symbol_id, bool is_stub);

	/** Get files where symbols have a status flag = 0 (not ready). */
	std::vector<std::string> getUnreadyFiles(uint64_t project_id,
						 const char *ready_field);

	/** Get ratio of symbols with a status flag = 1 (0.0 - 1.0). */
	double getReadyRatio(uint64_t project_id, const char *ready_field);

	// ── Incremental Indexing ──────────────────────────────────

	bool isFileUnchanged(uint64_t project_id, const char *file_path,
			     int64_t mtime, int64_t size);
	void updateFileScanState(uint64_t project_id, const char *file_path,
				 int64_t mtime, int64_t size);
	void cleanupStaleFiles(uint64_t project_id,
			       const std::vector<std::string> &active_files);

	// ── Path Tracing (BFS on call_edges) ──────────────────────

	/**
     * Trace the shortest call path between two functions using BFS on call_edges.
     * Returns JSON: {"path": [{"name":"...","file":"...","line":N}, ...]}
     * Requires callgraph_ready (enhancement must be run first).
     */
	std::string tracePathJson(uint64_t project_id, const char *from_name,
				  const char *to_name);

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
     * based on embedding_ready ratio.
     */
	std::string searchUnifiedJson(uint64_t project_id, const char *query,
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
	 * Find callers of a function by querying semantic_records directly.
	 * No pre-built call edges needed — builds the result on-the-fly in SQL.
	 * Returns JSON: {"callers":[{"name":"...","file":"...","line":...},...]}
	 * Returns "[]" if no callers found.
	 */
	std::string getCallersFromRecords(uint64_t project_id,
					  const char *function_name);

	/**
	 * Find callees of a function by querying semantic_records directly.
	 * Returns JSON: {"callees":[{"name":"...","file":"...","line":...},...]}
	 * Returns "[]" if no callees found.
	 */
	std::string getCalleesFromRecords(uint64_t project_id,
					  const char *function_name);

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

    private:
	sqlite3 *db_ = nullptr;
	std::string error_;

	// Cached prepared statements (initialized in open(), finalized in close())
	sqlite3_stmt *stmt_fts_map_ = nullptr; // INSERT INTO fts_node_map
	sqlite3_stmt *stmt_fts_ = nullptr; // INSERT INTO code_fts
	sqlite3_stmt *stmt_vector_ = nullptr; // INSERT INTO node_vectors

	bool exec(const char *sql);
	bool createSchema();
};

} // namespace store

#endif // STORE_H
