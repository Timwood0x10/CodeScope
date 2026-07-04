#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <unistd.h>
#include <string>
#include <sys/stat.h>

using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point start) {
    auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static size_t fileSize(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return static_cast<size_t>(st.st_size);
}

static void benchDef(const char* label, uint64_t pid, const char* name) {
    auto q0 = Clock::now();
    char* r = engine_find_definition(pid, name, nullptr);
    double ms = elapsed(q0);
    printf("query[def/%s]: %.3f ms, chars=%zu\n", label, ms, strlen(r));
    engine_free_string(r);
}

static void benchRefs(const char* label, uint64_t pid, const char* name) {
    auto q0 = Clock::now();
    char* r = engine_find_references(pid, name, nullptr);
    double ms = elapsed(q0);
    printf("query[refs/%s]: %.3f ms, chars=%zu\n", label, ms, strlen(r));
    engine_free_string(r);
}

static void benchCallers(const char* label, uint64_t pid, const char* name) {
    auto q0 = Clock::now();
    char* r = engine_get_callers(pid, name);
    double ms = elapsed(q0);
    printf("query[callers/%s]: %.3f ms, chars=%zu\n", label, ms, strlen(r));
    engine_free_string(r);
}

static void benchCallees(const char* label, uint64_t pid, const char* name) {
    auto q0 = Clock::now();
    char* r = engine_get_callees(pid, name);
    double ms = elapsed(q0);
    printf("query[callees/%s]: %.3f ms, chars=%zu\n", label, ms, strlen(r));
    engine_free_string(r);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source_file.go|source_file.py>\n", argv[0]);
        return 1;
    }

    const char* file_path = argv[1];
    unlink("/tmp/bench.db");

    auto init0 = Clock::now();
    int rc = engine_init("/tmp/bench.db");
    printf("engine_init: %.3f ms (rc=%d)\n", elapsed(init0), rc);
    if (rc != 0) { fprintf(stderr, "engine_init failed\n"); return 1; }

    uint64_t pid = engine_create_project("/tmp", "bench");
    printf("project_id: %llu\n", (unsigned long long)pid);

    size_t src_size = fileSize(file_path);
    printf("source_size: %zu bytes (%.1f KB)\n", src_size, src_size / 1024.0);

    auto idx0 = Clock::now();
    char* result = engine_index_file(pid, file_path);
    double ms = elapsed(idx0);
    printf("\n=== INDEX ===\n");
    printf("index_time: %.2f ms\n", ms);
    printf("throughput: %.1f KB/s\n", (src_size / 1024.0) / (ms / 1000.0));
    printf("result: %s (chars=%zu)\n", result, strlen(result));

    auto qt0 = Clock::now();

    benchDef("NewTokenizer", pid, "NewTokenizer");
    benchDef("Process", pid, "Process");
    benchDef("main", pid, "main");
    benchRefs("Add", pid, "Add");
    benchRefs("NewTokenizer", pid, "NewTokenizer");
    benchCallers("Process", pid, "Process");
    benchCallers("AnalyzeTokens", pid, "AnalyzeTokens");
    benchCallees("main", pid, "main");
    benchCallees("Tokenize", pid, "Tokenize");

    printf("\ntotal_query_time: %.2f ms\n", elapsed(qt0));

    char* stats = engine_get_graph_stats(pid);
    printf("stats: %s\n", stats);

    printf("\n=== TOKEN CONSUMPTION ===\n");
    printf("source_chars:    %zu\n", src_size);
    printf("index_result:    %zu chars\n", strlen(result));
    printf("stats_result:    %zu chars\n", strlen(stats));

    engine_free_string(stats);
    engine_free_string(result);
    engine_shutdown();

    return 0;
}
