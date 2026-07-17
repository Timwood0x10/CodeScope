/**
 * Quick verification: run CodeScope on AIScope to prove JS/TS pipeline works.
 * Usage: ./test_verify_aiscope <grammars_dir> <aiscope_src_dir>
 */
#include "../include/engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <grammars_dir> <aiscope_src_dir>\n", argv[0]);
        return 1;
    }
    setenv("GRAMMARS_DIR", argv[1], 1);

    unlink("/tmp/aiscope_test.db");

    int rc = engine_init("/tmp/aiscope_test.db");
    printf("engine_init: %d\n", rc);
    if (rc != 0) { fprintf(stderr, "FAIL: engine_init\n"); return 1; }

    uint64_t pid = engine_create_project(argv[2], "AIScope");
    printf("project_id: %llu\n", (unsigned long long)pid);
    if (pid == 0) { fprintf(stderr, "FAIL: create_project\n"); return 1; }

    printf("\n─── Indexing AIScope ───\n");
    char *result = engine_index_project(pid, argv[2], "typescript,tsx,javascript");
    printf("index_result: %s\n", result);
    if (!strstr(result, "\"ok\":true")) {
        fprintf(stderr, "FAIL: index_project\n");
        engine_free_string(result);
        engine_shutdown();
        return 1;
    }
    engine_free_string(result);

    printf("\n─── Graph Stats ───\n");
    char *stats = engine_get_graph_stats(pid);
    printf("stats: %s\n", stats);
    engine_free_string(stats);

    printf("\n─── Definitions ───\n");
    const char *defs[] = {"App", "useStore", "Home", "Attention", "Math", "RAG", "Agent", "NetworkBuilder"};
    for (auto name : defs) {
        char *def = engine_find_definition(pid, name, nullptr);
        bool found = strstr(def, "\"symbol\":") != nullptr;
        printf("  def[%s]: %s\n", name, found ? "FOUND" : "NOT FOUND");
        engine_free_string(def);
    }

    printf("\n─── Callers (Router -> pages) ───\n");
    const char *calls[] = {"Home", "Attention", "Math"};
    for (auto name : calls) {
        char *c = engine_get_callers(pid, name, nullptr);
        printf("  callers[%s]: %s\n", name, c);
        engine_free_string(c);
    }

    engine_shutdown();
    printf("\n=== AIScope verification PASSED ===\n");
    return 0;
}
