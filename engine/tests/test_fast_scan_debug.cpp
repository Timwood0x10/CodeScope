/**
 * Debug: run fast scanner on full Linux kernel
 */
#include "../include/engine.h"
#include <cstdio>
#include <chrono>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

int main(int argc, char **argv) {
    fprintf(stderr, "DEBUG: starting\n");
    if (argc < 3) { fprintf(stderr, "usage: %s <grammars> <kernel_dir>\n", argv[0]); return 1; }
    setenv("GRAMMARS_DIR", argv[1], 1);

    unlink("/tmp/fast_scan_debug.db");
    fprintf(stderr, "DEBUG: init...\n");
    int rc = engine_init("/tmp/fast_scan_debug.db");
    fprintf(stderr, "DEBUG: init=%d\n", rc);
    if (rc != 0) return 1;

    uint64_t pid = engine_create_project(argv[2], "linux");
    fprintf(stderr, "DEBUG: pid=%llu\n", (unsigned long long)pid);
    if (pid == 0) { fprintf(stderr, "FAIL: create_project\n"); return 1; }

    fprintf(stderr, "DEBUG: scanning...\n");
    auto t0 = Clock::now();
    char *result = engine_scan_project(pid, argv[2], "c");
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    fprintf(stderr, "DEBUG: done in %.0f ms\n", ms);

    fprintf(stderr, "result (first 200): %.*s\n", 200, result ? result : "NULL");
    engine_free_string(result);
    engine_shutdown();
    return 0;
}
