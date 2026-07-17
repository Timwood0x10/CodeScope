#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

int main() {
    const char *db = "/tmp/check_graph.db";
    unlink(db); unlink("/tmp/check_graph.db.lbug"); unlink("/tmp/astgraph_test.db");
    engine_init(db);
    uint64_t pid = engine_create_project(
        "/Users/scc/code/pycode/Transformer_Explorer", "check");
    char *idx = engine_index_project(pid,
        "/Users/scc/code/pycode/Transformer_Explorer", nullptr);
    if (idx) engine_free_string(idx);
    usleep(800000);

    sqlite3 *h = nullptr;
    sqlite3_open(db, &h);

    // 1. graph_edges 总数
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM graph_edges WHERE project_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, pid);
    if (sqlite3_step(st) == SQLITE_ROW) printf("graph_edges total: %d\n", sqlite3_column_int(st, 0));
    sqlite3_finalize(st);

    // 2. resolve_strategy 在 graph_edges 中的分布
    sqlite3_prepare_v2(h,
        "SELECT resolve_strategy, COUNT(*) FROM graph_edges "
        "WHERE project_id=? AND resolve_strategy != '' GROUP BY resolve_strategy",
        -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, pid);
    printf("\nresolve_strategy distribution in graph_edges:\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        printf("  %s → %d edges\n",
               sqlite3_column_text(st, 0),
               sqlite3_column_int(st, 1));
    }
    sqlite3_finalize(st);

    // 3. graph_nodes 总数
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM graph_nodes WHERE project_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, pid);
    if (sqlite3_step(st) == SQLITE_ROW) printf("\ngraph_nodes total: %d\n", sqlite3_column_int(st, 0));
    sqlite3_finalize(st);

    // 4. graph_nodes 中 node_type 分布
    sqlite3_prepare_v2(h,
        "SELECT node_type, COUNT(*) FROM graph_nodes "
        "WHERE project_id=? GROUP BY node_type ORDER BY node_type",
        -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, pid);
    printf("\ngraph_nodes node_type distribution:\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        printf("  type=%d → %d nodes\n",
               sqlite3_column_int(st, 0),
               sqlite3_column_int(st, 1));
    }
    sqlite3_finalize(st);

    sqlite3_close(h);
    engine_shutdown();
    printf("\n=== graph check done ===\n");
    return 0;
}
