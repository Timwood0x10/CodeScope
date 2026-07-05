/**
 * Benchmark: analyze a real-world project with the new JS/TS pipeline.
 * Measures wall-clock time, peak RSS, and CPU utilization.
 *
 * Usage: ./test_bench_project <grammars_dir> <project_dir> [lang_filter] [max_file_size_mb]
 */
#include "../include/engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
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

static double cpu_seconds() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        return static_cast<double>(usage.ru_utime.tv_sec) +
               static_cast<double>(usage.ru_utime.tv_usec) / 1e6 +
               static_cast<double>(usage.ru_stime.tv_sec) +
               static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <grammars_dir> <project_dir> [lang_filter] [max_file_size_mb]\n", argv[0]);
        return 1;
    }
    setenv("GRAMMARS_DIR", argv[1], 1);

    // Optional: set max file size (default 1MB to skip huge fixtures)
    int max_mb = (argc > 3) ? atoi(argv[3]) : 1;
    if (max_mb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", max_mb * 1024 * 1024);
        setenv("CODESCOPE_MAX_FILE_SIZE", buf, 1);
        printf("max_file_size: %d MB\n", max_mb);
    }

    const char *lang_filter = (argc > 4) ? argv[4] : "typescript,tsx,javascript";
    printf("lang_filter: %s\n", lang_filter);
    printf("project_dir: %s\n", argv[2]);

    // Count source files first
    std::string count_cmd = std::string("find ") + argv[2] +
        " -name \"*.ts\" -o -name \"*.tsx\" -o -name \"*.js\" 2>/dev/null | "
        "grep -v node_modules | wc -l";
    FILE *fp = popen(count_cmd.c_str(), "r");
    char count_buf[64] = {0};
    if (fp) { fgets(count_buf, sizeof(count_buf), fp); pclose(fp); }
    int total_files = atoi(count_buf);
    printf("total_ts_js_files: %d\n", total_files);

    std::string size_cmd = std::string("find ") + argv[2] +
        " -name \"*.ts\" -o -name \"*.tsx\" -o -name \"*.js\" 2>/dev/null | "
        "grep -v node_modules | xargs wc -l 2>/dev/null | tail -1";
    fp = popen(size_cmd.c_str(), "r");
    char size_buf[64] = {0};
    if (fp) { fgets(size_buf, sizeof(size_buf), fp); pclose(fp); }
    printf("total_lines: %s", size_buf);

    unlink("/tmp/bench_project.db");

    size_t rss_before = peak_rss_kb();
    double cpu_before = cpu_seconds();

    auto t0 = Clock::now();
    int rc = engine_init("/tmp/bench_project.db");
    double init_ms = elapsed_ms(t0);
    printf("\nengine_init: %d (%.0f ms)\n", rc, init_ms);
    if (rc != 0) { fprintf(stderr, "FAIL: engine_init\n"); return 1; }

    t0 = Clock::now();
    uint64_t pid = engine_create_project(argv[2], "bench-project");
    double create_ms = elapsed_ms(t0);
    printf("create_project: %llu (%.0f ms)\n", (unsigned long long)pid, create_ms);

    // Index
    printf("\n─── INDEXING ───\n");
    t0 = Clock::now();
    char *result = engine_index_project(pid, argv[2], lang_filter);
    double index_ms = elapsed_ms(t0);

    size_t rss_after = peak_rss_kb();
    double cpu_after = cpu_seconds();

    printf("index_time: %.0f ms (%.2f s)\n", index_ms, index_ms / 1000.0);
    printf("result: %s\n", result);

    int files_idx = 0;
    if (strstr(result, "\"files_indexed\":"))
        sscanf(strstr(result, "\"files_indexed\":"), "\"files_indexed\":%d", &files_idx);
    engine_free_string(result);

    // Stats
    printf("\n─── GRAPH STATS ───\n");
    t0 = Clock::now();
    char *stats = engine_get_graph_stats(pid);
    double query_ms = elapsed_ms(t0);
    printf("stats: %s (%.0f ms)\n", stats, query_ms);
    engine_free_string(stats);

    // Summary
    printf("\n═══ PERFORMANCE SUMMARY ═══\n");
    printf("files_indexed:  %d / %d\n", files_idx, total_files);
    printf("wall_time:      %.2f s\n", index_ms / 1000.0);
    printf("peak_rss:       %zu KB (%.1f MB)\n", rss_after, rss_after / 1024.0);
    printf("rss_delta:      %zu KB (%.1f MB)\n", rss_after - rss_before, (rss_after - rss_before) / 1024.0);
    printf("cpu_time:       %.2f s\n", cpu_after - cpu_before);
    printf("throughput:     %.0f files/min\n", files_idx / (index_ms / 1000.0 / 60.0));
    printf("throughput:     %.0f lines/sec\n", atof(size_buf) / (index_ms / 1000.0));

    // Connection count
    if (files_idx > 0 && index_ms > 0) {
        printf("avg_time_per_file: %.0f ms\n", index_ms / files_idx);
    }

    engine_shutdown();
    printf("\n=== Benchmark complete ===\n");
    return 0;
}
