#include "../include/engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <string>

using Clock = std::chrono::steady_clock;

static double elapsed_s(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

int main(int argc, char** argv) {
    const char* project_dir = argc > 1 ? argv[1] : ".";
    const char* lang_filter = argc > 2 ? argv[2] : "typescript,tsx,javascript,go,c,cpp,rust,python,java";

    unlink("/tmp/bench_enhance.db");
    unlink("/tmp/bench_enhance.db-wal");
    unlink("/tmp/bench_enhance.db-shm");

    setenv("CODESCOPE_VERBOSE", "1", 1);

    int rc = engine_init("/tmp/bench_enhance.db");
    if (rc != 0) { fprintf(stderr, "FAIL: engine_init\n"); return 1; }
    fprintf(stderr, "engine_init OK\n");

    uint64_t pid = engine_create_project(project_dir, "bench-enhance");
    if (pid == 0) { fprintf(stderr, "FAIL: create_project\n"); return 1; }
    fprintf(stderr, "create_project OK (pid=%llu)\n", (unsigned long long)pid);

    fprintf(stderr, "\n=== INDEX ===\n");
    auto t0 = Clock::now();
    char* result = engine_index_project(pid, project_dir, lang_filter);
    double index_s = elapsed_s(t0);
    fprintf(stderr, "index: %.2f s\n", index_s);
    fprintf(stderr, "result: %s\n", result);

    bool ok = strstr(result, "\"ok\":true") != nullptr;
    engine_free_string(result);
    if (!ok) { fprintf(stderr, "FAIL: index\n"); engine_shutdown(); return 1; }

    fprintf(stderr, "\n=== ENHANCE (default mode — skip_vectors) ===\n");
    t0 = Clock::now();
    char* enhance_result = engine_enhance_project(pid);
    double enhance_s = elapsed_s(t0);
    fprintf(stderr, "enhance: %.2f s\n", enhance_s);
    fprintf(stderr, "enhance result: %s\n", enhance_result);
    engine_free_string(enhance_result);

    fprintf(stderr, "\n=== ENHANCE AGAIN (idempotent — should be 0) ===\n");
    t0 = Clock::now();
    enhance_result = engine_enhance_project(pid);
    double enhance2_s = elapsed_s(t0);
    fprintf(stderr, "enhance2: %.2f s\n", enhance2_s);
    fprintf(stderr, "enhance2 result: %s\n", enhance_result);
    engine_free_string(enhance_result);

    fprintf(stderr, "\n=== ENHANCE DEEP (with vectors) ===\n");
    setenv("CODESCOPE_ENHANCE_MODE", "deep", 1);
    t0 = Clock::now();
    enhance_result = engine_enhance_project(pid);
    double enhance_deep_s = elapsed_s(t0);
    fprintf(stderr, "enhance deep: %.2f s\n", enhance_deep_s);
    fprintf(stderr, "enhance deep result: %s\n", enhance_result);
    engine_free_string(enhance_result);

    char* status = engine_get_enhancement_status(pid);
    fprintf(stderr, "\nstatus: %s\n", status);
    engine_free_string(status);

    engine_shutdown();

    fprintf(stderr, "\n=== SUMMARY ===\n");
    fprintf(stderr, "index:          %.2f s\n", index_s);
    fprintf(stderr, "enhance:        %.2f s\n", enhance_s);
    fprintf(stderr, "enhance deep:   %.2f s\n", enhance_deep_s);
    fprintf(stderr, "total:          %.2f s\n", index_s + enhance_s + enhance_deep_s);
    return 0;
}
