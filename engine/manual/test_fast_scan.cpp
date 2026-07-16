/**
 * Test the fast scanner (engine_scan_project) on the Linux kernel.
 * Quick benchmark: how fast can it scan 60K C files?
 */
#include "../include/engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/resource.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

static double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static size_t peak_rss_kb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        return static_cast<size_t>(usage.ru_maxrss) / 1024;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <grammars_dir> <kernel_dir>\n", argv[0]);
        return 1;
    }
    setenv("GRAMMARS_DIR", argv[1], 1);

    unlink("/tmp/fast_scan.db");

    size_t rss_before = peak_rss_kb();
    auto t0 = Clock::now();
    int rc = engine_init("/tmp/fast_scan.db");
    printf("engine_init: %d (%.0f ms)\n", rc, elapsed_ms(t0));
    if (rc != 0) return 1;

    uint64_t pid = engine_create_project(argv[2], "linux-kernel");
    printf("project_id: %llu\n", (unsigned long long)pid);

    // Count C files
    std::string cmd = std::string("find ") + argv[2] +
        " -name \"*.c\" -o -name \"*.h\" 2>/dev/null | wc -l";
    FILE *fp = popen(cmd.c_str(), "r");
    char buf[64] = {0};
    if (fp) { fgets(buf, sizeof(buf), fp); pclose(fp); }

    printf("\n─── FAST SCAN (engine_scan_project) ───\n");
    printf("C/H files: %s", buf);

    // Time just the file enumeration and gitignore loading
    t0 = Clock::now();
    char *result = engine_scan_project(pid, argv[2], "c");
    double scan_ms = elapsed_ms(t0);

    size_t rss_after = peak_rss_kb();

    printf("scan_time: %.2f ms (%.2f s)\n", scan_ms, scan_ms / 1000.0);
    printf("result: %s\n", result);
    engine_free_string(result);

    printf("\n═══ RESULTS ═══\n");
    printf("wall_time:  %.2f s\n", scan_ms / 1000.0);
    printf("peak_rss:   %zu MB\n", rss_after / 1024);
    printf("rss_delta:  %zu MB\n", (rss_after - rss_before) / 1024);

    engine_shutdown();
    printf("\n=== Fast scan complete ===\n");
    return 0;
}
