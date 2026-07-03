#ifndef STORE_H
#define STORE_H

#include <cstdint>
#include <string>
#include <vector>

#include "../graph/graph_types.h"

struct sqlite3;

namespace store {

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
                        const char *qualified_name, uint32_t sr, uint32_t sc,
                        uint32_t er, uint32_t ec, const char *language);

  bool insertIRSemanticEdge(uint64_t project_id, uint64_t source_id,
                            uint64_t target_id, int relation);

  bool deleteIRByFile(uint64_t project_id, uint64_t file_id);

  // ── Graph Nodes ────────────────────────────────────────────

  uint64_t insertGraphNode(uint64_t project_id, const graph::GraphNode &node);
  bool deleteGraphNodesByFile(uint64_t project_id, const char *file_path);

  // ── Graph Edges ────────────────────────────────────────────

  uint64_t insertGraphEdge(uint64_t project_id, const graph::GraphEdge &edge);
  bool deleteGraphEdgesByFile(uint64_t project_id, const char *file_path);

  // ── Transactions ───────────────────────────────────────────

  bool beginTransaction();
  bool commitTransaction();
  bool rollbackTransaction();

  // ── Search ─────────────────────────────────────────────────

  std::string searchCode(uint64_t project_id, const char *query, int limit);

  // ── FTS index helpers ───────────────────────────────────────

  void insertIntoFTS(uint64_t node_id, uint64_t project_id, const char *name,
                     const char *qualified_name, const char *file_path,
                     const char *content, int node_kind = -1);

  // ── Vector search (semantic) ─────────────────────────────────

  bool storeVector(uint64_t node_id, uint64_t project_id, const void *vec_data,
                   size_t vec_bytes);
  std::string searchSemantic(uint64_t project_id, const void *query_vec,
                             size_t vec_bytes, int limit);
  void deleteFTSByFile(uint64_t project_id, uint64_t file_id);

  // ── Complexity ───────────────────────────────────────────────

  bool setComplexity(uint64_t project_id, uint64_t graph_node_id,
                     uint64_t cyclomatic, uint64_t cognitive,
                     uint64_t nesting_depth, uint64_t decision_points);
  std::string getComplexityJson(uint64_t project_id, uint64_t graph_node_id);

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
                        const char *language, const char *file_path, int line,
                        int column, int span_start, int span_end);

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
                          uint64_t callee_symbol_id, const char *provenance,
                          int line, int col);

  uint64_t insertDependencyEdge(uint64_t project_id, uint64_t source_module_id,
                                uint64_t target_module_id,
                                const char *external_name, const char *kind);

  // ── Phase B: Enhancement — Metrics ────────────────────────

  bool insertMetric(uint64_t project_id, const char *owner_type,
                    uint64_t owner_id, int cyclomatic, int nesting_depth,
                    int cognitive, int lines, int param_count, int call_count,
                    int branch_count, int loop_count);

  // ── Phase B: Enhancement — Search Index ───────────────────

  void insertIntoSearchIndex(uint64_t symbol_id, uint64_t project_id,
                             const char *title, const char *summary,
                             const char *body);

  // ── Phase B: Enhancement — Embeddings ─────────────────────

  bool insertEmbedding(uint64_t symbol_id, const float *vector_data, int dim);

  // ── Symbol Status (separate from symbols, keeps main table lean) ──

  /** Set a specific status flag (callgraph_ready/metrics_ready/embedding_ready)
   * to 1. */
  bool setSymbolReady(uint64_t symbol_id, const char *field);

  /** Get files where symbols have a status flag = 0 (not ready). */
  std::vector<std::string> getUnreadyFiles(uint64_t project_id,
                                           const char *ready_field);

  /** Get ratio of symbols with a status flag = 1 (0.0 - 1.0). */
  double getReadyRatio(uint64_t project_id, const char *ready_field);

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
  std::string findCallersJson(uint64_t project_id, const char *symbol_name);

  /**
   * Find callees from the new call_edges table (requires callgraph_ready).
   * Returns JSON array of callee symbols.
   */
  std::string findCalleesJson(uint64_t project_id, const char *symbol_name);

  /** Get entry points from the new entry_points table. */
  std::string getEntryPointsJson(uint64_t project_id);

  // ── Raw access (for query engine) ──────────────────────────

  sqlite3 *handle() const { return db_; }

  const std::string &error() const { return error_; }

private:
  sqlite3 *db_ = nullptr;
  std::string error_;

  bool exec(const char *sql);
  bool createSchema();
};

} // namespace store

#endif // STORE_H
