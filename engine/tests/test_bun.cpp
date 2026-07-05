#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <unistd.h>
#include <string>

using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point start) {
    auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main(int argc, char** argv) {
    const char* bun_dir = argc > 1 ? argv[1] : "/Users/scc/code/researcher/bun";

    unlink("/tmp/test_bun.db");

    int rc = engine_init("/tmp/test_bun.db");
    if (rc != 0) {
        fprintf(stderr, "FAIL: engine_init\n");
        return 1;
    }
    fprintf(stderr, "engine_init OK\n");

    uint64_t pid = engine_create_project("/tmp", "bun-test");
    if (pid == 0) {
        fprintf(stderr, "FAIL: create_project\n");
        return 1;
    }
    fprintf(stderr, "create_project OK (pid=%llu)\n", (unsigned long long)pid);

    fprintf(stderr, "Indexing %s ...\n", bun_dir);
    auto t0 = Clock::now();
    char* result = engine_index_project(pid, bun_dir, nullptr);
    double ms = elapsed(t0);
    fprintf(stderr, "index_project: %.0f ms\n", ms);

    if (!result) {
        fprintf(stderr, "FAIL: index_project returned null\n");
        engine_shutdown();
        return 1;
    }

    fprintf(stderr, "Result: %s\n", result);

    bool ok = strstr(result, "\"ok\":true") != nullptr;
    engine_free_string(result);

    if (!ok) {
        fprintf(stderr, "FAIL: index_project failed\n");
        engine_shutdown();
        return 1;
    }

    fprintf(stderr, "\n=== bun index test passed ===\n");
    engine_shutdown();
    return 0;
}
