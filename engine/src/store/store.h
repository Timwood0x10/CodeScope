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

    GraphStore(const GraphStore&) = delete;
    GraphStore& operator=(const GraphStore&) = delete;

    bool open(const char* db_path);
    void close();

    // ── Project ────────────────────────────────────────────────

    uint64_t createProject(const char* root_path, const char* name);
    uint64_t getProjectId(const char* root_path);

    // ── File ───────────────────────────────────────────────────

    uint64_t upsertFile(uint64_t project_id, const char* path,
                        const char* language, const char* content_hash);

    // ── IR Nodes ───────────────────────────────────────────────

    uint64_t insertIRNode(uint64_t project_id, uint64_t file_id,
                          uint64_t parent_id, int kind,
                          const char* name, const char* qualified_name,
                          uint32_t sr, uint32_t sc, uint32_t er, uint32_t ec,
                          const char* language);

    bool insertIRSemanticEdge(uint64_t project_id,
                              uint64_t source_id, uint64_t target_id,
                              int relation);

    bool deleteIRByFile(uint64_t project_id, uint64_t file_id);

    // ── Graph Nodes ────────────────────────────────────────────

    uint64_t insertGraphNode(uint64_t project_id, const graph::GraphNode& node);
    bool deleteGraphNodesByFile(uint64_t project_id, const char* file_path);

    // ── Graph Edges ────────────────────────────────────────────

    uint64_t insertGraphEdge(uint64_t project_id, const graph::GraphEdge& edge);
    bool deleteGraphEdgesByFile(uint64_t project_id, const char* file_path);

    // ── Transactions ───────────────────────────────────────────

    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // ── Search ─────────────────────────────────────────────────

    std::string searchCode(uint64_t project_id, const char* query, int limit);

    // ── FTS index helpers ───────────────────────────────────────

    void insertIntoFTS(uint64_t node_id, uint64_t project_id,
                       const char* name, const char* qualified_name,
                       const char* file_path, const char* content);
    void deleteFTSByFile(uint64_t project_id, uint64_t file_id);

    // ── Complexity ───────────────────────────────────────────────

    bool setComplexity(uint64_t project_id, uint64_t graph_node_id,
                       uint64_t cyclomatic, uint64_t cognitive,
                       uint64_t nesting_depth, uint64_t decision_points);
    std::string getComplexityJson(uint64_t project_id, uint64_t graph_node_id);

    // ── Raw access (for query engine) ──────────────────────────

    sqlite3* handle() const { return db_; }

    const std::string& error() const { return error_; }

private:
    sqlite3*    db_ = nullptr;
    std::string error_;

    bool exec(const char* sql);
    bool createSchema();
};

} // namespace store

#endif // STORE_H
