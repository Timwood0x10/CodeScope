#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>

static void dump(const char *label, char *s)
{
    printf("--- %s ---\n%s\n\n", label, s ? s : "(null)");
    if (s) engine_free_string(s);
}

int main(int argc, char **argv)
{
    const char *bun_dir = argc > 1 ? argv[1] : "/Users/scc/code/researcher/bun";
    const char *db = "/tmp/t_bun.db";
    unlink(db); unlink("/tmp/astgraph_test.db");

    engine_init(db);
    uint64_t pid = engine_create_project(bun_dir, "bun");
    char *idx = engine_index_project(pid, bun_dir, nullptr);
    printf("index: %s\n", idx ? idx : "(null)");
    if (idx) engine_free_string(idx);

    for (int i = 0; i < 100; i++) {
        usleep(100000);
        char *st = engine_get_graph_stats(pid);
        if (st && strstr(st, "total_nodes")) {
            engine_free_string(st);
            break;
        }
        if (st) engine_free_string(st);
    }
    usleep(500000);

    dump("stats", engine_get_graph_stats(pid));

    // Check some key functions
    const char *funcs[] = {"main", "run", "init", nullptr};
    for (int i = 0; funcs[i]; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "callees(%s)", funcs[i]);
        dump(buf, engine_get_callees(pid, funcs[i], nullptr));
        snprintf(buf, sizeof(buf), "callers(%s)", funcs[i]);
        dump(buf, engine_get_callers(pid, funcs[i], nullptr));
    }

    // Dump resolve_strategy from semantic_records
    sqlite3 *h = nullptr;
    sqlite3_open(db, &h);
    printf("=== resolve_strategy sample (first 20) ===\n");
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(h,
        "SELECT rowid, name, resolve_strategy, ref_original_id, "
        "start_row, file_path "
        "FROM semantic_records "
        "WHERE project_id=? AND kind=9 AND name != '' "
        "AND resolve_strategy != '' "
        "ORDER BY start_row LIMIT 20",
        -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, pid);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 1);
        const char *rs = (const char *)sqlite3_column_text(st, 2);
        int ref = sqlite3_column_int(st, 3);
        int row = sqlite3_column_int(st, 4);
        const char *fp = (const char *)sqlite3_column_text(st, 5);
        printf("  row=%-5d name=%-20s strategy=%-12s ref_oid=%-2d %s\n",
               row, n ? n : "", rs ? rs : "", ref, fp ? fp : "");
    }
    sqlite3_finalize(st);
    sqlite3_close(h);

    engine_shutdown();
    printf("=== DONE ===\n");
    return 0;
}
