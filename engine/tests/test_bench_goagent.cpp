// test_bench_goagent.cpp — indexes ~/go/src/goagent and records timing
#include "../include/engine.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int main() {
    using Clock = std::chrono::steady_clock;
    char db_path[] = "/tmp/test_goagent_bench.db";
    unlink(db_path);

    auto t0 = Clock::now();
    int rc = engine_init(db_path);
    if (rc != 0) { fprintf(stderr, "FAIL: engine_init\n"); return 1; }
    auto t1 = Clock::now();

    uint64_t pid = engine_create_project("/tmp", "goagent-bench");
    if (pid == 0) { fprintf(stderr, "FAIL: create_project\n"); return 1; }
    auto t2 = Clock::now();

    fprintf(stderr, "Indexing ~/go/src/goagent ...\n");
    auto t_parse_start = Clock::now();
    char *result = engine_index_project(pid, "/Users/scc/go/src/goagent", nullptr);
    auto t_parse_end = Clock::now();
    if (!result || !strstr(result, "\"ok\":true")) {
        fprintf(stderr, "FAIL: index\n"); return 1;
    }

    // Parse result JSON for timing data
    auto parseVal = [&](const char *key) -> int64_t {
        const char *p = strstr(result, key);
        if (!p) return -1;
        p = strchr(p, ':');
        if (!p) return -1;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        return std::atoll(p);
    };
    int64_t files = parseVal("\"files_indexed\"");
    int64_t t_parse = parseVal("\"time_parse_ms\"");
    int64_t t_build = parseVal("\"time_buildgraph_ms\"");
    int64_t n_nodes = parseVal("\"total_nodes\"");
    int64_t n_edges = parseVal("\"total_edges\"");
    int64_t n_calls = parseVal("\"total_call_edges\"");

    auto t_after_async = Clock::now();
    // Wait for async to finish
    usleep(2000000);
    auto t_end = Clock::now();

    fprintf(stderr, "\n=== goagent Index Result ===\n");
    fprintf(stderr, "  Files:       %lld\n", (long long)files);
    fprintf(stderr, "  Nodes:       %lld\n", (long long)n_nodes);
    fprintf(stderr, "  Edges:       %lld\n", (long long)n_edges);
    fprintf(stderr, "  Call edges:  %lld\n", (long long)n_calls);
    fprintf(stderr, "  Parse:       %lld ms\n", (long long)t_parse);
    fprintf(stderr, "  BuildGraph:  %lld ms\n", (long long)t_build);
    fprintf(stderr, "  Init:        %lld ms\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    fprintf(stderr, "  Index (parse+build): %lld ms\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t_parse_end - t_parse_start).count());
    fprintf(stderr, "  Async wait:  %lld ms\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_after_async).count());
    fprintf(stderr, "  Wall clock:  %lld ms\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t0).count());

    engine_free_string(result);
    engine_shutdown();
    unlink(db_path);

    fprintf(stderr, "\n=== README reference: goagent 2,651 files, 155K nodes, 30s ===\n");
    return 0;
}