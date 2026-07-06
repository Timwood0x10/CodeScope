/**
 * Benchmark: analyze a real-world project with the new JS/TS pipeline.
 * Measures wall-clock time, peak RSS, CPU utilization, and per-phase breakdown.
 *
 * Usage: ./test_bench_project <grammars_dir> <project_dir> [max_file_size_mb] [lang_filter]
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

/**
 * Extract an int64 value from a JSON key, e.g. extract_int64(json, "files_indexed")
 * Returns 0 if key not found.
 */
static int64_t extract_int64(const char *json, const char *key) {
    char buf[64];
    snprintf(buf, sizeof(buf), "\"%s\":", key);
    const char *p = strstr(json, buf);
    if (!p) return 0;
    p += strlen(buf);
    return static_cast<int64_t>(atoll(p));
}

static double extract_double(const char *json, const char *key) {
    char buf[64];
    snprintf(buf, sizeof(buf), "\"%s\":", key);
    const char *p = strstr(json, buf);
    if (!p) return 0.0;
    p += strlen(buf);
    return atof(p);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <grammars_dir> <project_dir> [max_file_size_mb] [lang_filter]\n", argv[0]);
        return 1;
    }
    setenv("GRAMMARS_DIR", argv[1], 1);
    // Suppress batch-level stderr logging during benchmark
    setenv("CODESCOPE_VERBOSE", "0", 1);

    // Optional: set max file size (default 1MB to skip huge fixtures)
    int max_mb = (argc > 3) ? atoi(argv[3]) : 1;
    if (max_mb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", max_mb * 1024 * 1024);
        setenv("CODESCOPE_MAX_FILE_SIZE", buf, 1);
        printf("max_file_size: %d MB\n", max_mb);
    }

    const char *lang_filter = (argc > 4) ? argv[4] : "typescript,tsx,javascript";
    printf("lang_filter:     %s\n", lang_filter);
    printf("project_dir:     %s\n", argv[2]);

    // Map language filter to file extensions for counting
    std::string ext_filter;
    if (strcmp(lang_filter, "c") == 0 || strcmp(lang_filter, "cpp") == 0)
        ext_filter = "-name \"*.c\" -o -name \"*.h\" -o -name \"*.cpp\" -o -name \"*.hpp\"";
    else if (strcmp(lang_filter, "go") == 0)
        ext_filter = "-name \"*.go\"";
    else if (strcmp(lang_filter, "rust") == 0)
        ext_filter = "-name \"*.rs\"";
    else if (strcmp(lang_filter, "python") == 0)
        ext_filter = "-name \"*.py\"";
    else if (strcmp(lang_filter, "java") == 0)
        ext_filter = "-name \"*.java\"";
    else
        ext_filter = "-name \"*.ts\" -o -name \"*.tsx\" -o -name \"*.js\"";

    // Count source files first
    std::string count_cmd = std::string("find ") + argv[2] + " " + ext_filter +
        " 2>/dev/null | grep -v node_modules | wc -l";
    FILE *fp = popen(count_cmd.c_str(), "r");
    char count_buf[64] = {0};
    if (fp) { fgets(count_buf, sizeof(count_buf), fp); pclose(fp); }
    int total_files = atoi(count_buf);
    printf("total_source_files: %d\n", total_files);

    std::string size_cmd = std::string("find ") + argv[2] + " " + ext_filter +
        " 2>/dev/null | grep -v node_modules | xargs wc -l 2>/dev/null | tail -1";
    fp = popen(size_cmd.c_str(), "r");
    char size_buf[64] = {0};
    if (fp) { fgets(size_buf, sizeof(size_buf), fp); pclose(fp); }
    printf("total_lines:     %s", size_buf);

    unlink("/tmp/bench_project.db");
    unlink("/tmp/bench_project.db-wal");
    unlink("/tmp/bench_project.db-shm");

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
    printf("\n\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 INDEXING \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n");
    auto idx_start = Clock::now();
    char *result = engine_index_project(pid, argv[2], lang_filter);
    double index_ms = elapsed_ms(idx_start);

    size_t rss_after = peak_rss_kb();
    double cpu_after = cpu_seconds();

    printf("index_time:      %.0f ms (%.2f s)\n", index_ms, index_ms / 1000.0);
    printf("result:          %s\n", result);

    int files_idx = 0;
    if (strstr(result, "\"files_indexed\":"))
        sscanf(strstr(result, "\"files_indexed\":"), "\"files_indexed\":%d", &files_idx);

    // Parse per-phase timing from JSON result
    int64_t time_parse_ms = extract_int64(result, "time_parse_ms");
    int64_t time_sqlite_ms = extract_int64(result, "time_sqlite_ms");
    int64_t time_buildgraph_ms = extract_int64(result, "time_buildgraph_ms");
    int64_t time_ftsvector_ms = extract_int64(result, "time_ftsvector_ms");
    engine_free_string(result);

    // Phase breakdown
    int64_t time_accounted = time_parse_ms + time_sqlite_ms + time_buildgraph_ms + time_ftsvector_ms;
    int64_t time_unaccounted = static_cast<int64_t>(index_ms) - time_accounted;

    printf("\n\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 PHASE BREAKDOWN \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n");
    if (time_accounted > 0) {
        printf("%-20s %10s %8s\n", "Phase", "Time (ms)", "Percent");
        printf("%-20s %10s %8s\n", "--------------------", "----------", "--------");
        printf("%-20s %10lld %7.1f%%\n", "Parse (parallel)",
               (long long)time_parse_ms,
               100.0 * time_parse_ms / index_ms);
        printf("%-20s %10lld %7.1f%%\n", "SQLite (serial write)",
               (long long)time_sqlite_ms,
               100.0 * time_sqlite_ms / index_ms);
        printf("%-20s %10lld %7.1f%%\n", "buildGraph (post)",
               (long long)time_buildgraph_ms,
               100.0 * time_buildgraph_ms / index_ms);
        printf("%-20s %10lld %7.1f%%\n", "FTS/vectors (post)",
               (long long)time_ftsvector_ms,
               100.0 * time_ftsvector_ms / index_ms);
        if (time_unaccounted > 0) {
            printf("%-20s %10lld %7.1f%%\n", "(overhead/other)",
                   (long long)time_unaccounted,
                   100.0 * time_unaccounted / index_ms);
        }
        printf("%-20s %10s %8s\n", "--------------------", "----------", "--------");
        printf("%-20s %10lld %7.1f%%\n", "Total",
               (long long)index_ms, 100.0);
    } else {
        printf("(no per-phase timing available)\n");
    }

    // Stats
    printf("\n\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 GRAPH STATS \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n");
    t0 = Clock::now();
    char *stats = engine_get_graph_stats(pid);
    double query_ms = elapsed_ms(t0);
    printf("stats: %s (%.0f ms)\n", stats, query_ms);
    engine_free_string(stats);

    // Summary
    printf("\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90 PERFORMANCE SUMMARY \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n");
    printf("%-25s %s\n", "Metric", "Value");
    printf("%-25s %s\n", "-------------------------", "-------------------------");
    printf("%-25s %d / %d\n", "files_indexed", files_idx, total_files);
    printf("%-25s %.2f s\n", "wall_time", index_ms / 1000.0);
    printf("%-25s %.2f s\n", "cpu_time", cpu_after - cpu_before);
    printf("%-25s %zu KB (%.1f MB)\n", "peak_rss", rss_after, rss_after / 1024.0);
    printf("%-25s %zu KB (%.1f MB)\n", "rss_delta", rss_after - rss_before, (rss_after - rss_before) / 1024.0);

    if (files_idx > 0 && index_ms > 0) {
        printf("%-25s %.0f ms\n", "avg_time_per_file", index_ms / files_idx);
        printf("%-25s %.0f files/min\n", "throughput", files_idx / (index_ms / 1000.0 / 60.0));
        double lines_total = atof(size_buf);
        if (lines_total > 0) {
            printf("%-25s %.0f lines/sec\n", "throughput (lines)", lines_total / (index_ms / 1000.0));
        }
    }

    printf("\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90 QUERY BENCHMARK \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n");

    // searchCode benchmark
    {
        t0 = Clock::now();
        char *res = engine_search_code(pid, "function", 10);
        double t = elapsed_ms(t0);
        printf("%-35s %10.1f ms\n", "searchCode(\"function\",10)", t);
        engine_free_string(res);
    }

    // searchSemantic benchmark (uses vector search)
    {
        t0 = Clock::now();
        char *res = engine_search_semantic(pid, "function", 10);
        double t = elapsed_ms(t0);
        printf("%-35s %10.1f ms\n", "searchSemantic(\"function\",10)", t);
        engine_free_string(res);
    }

    // getCallers benchmark (from graph)
    {
        t0 = Clock::now();
        char *res = engine_get_callers(pid, "onRequest");
        double t = elapsed_ms(t0);
        printf("%-35s %10.1f ms\n", "getCallers(\"onRequest\")", t);
        engine_free_string(res);
    }

    // getCallees benchmark
    {
        t0 = Clock::now();
        char *res = engine_get_callees(pid, "onRequest");
        double t = elapsed_ms(t0);
        printf("%-35s %10.1f ms\n", "getCallees(\"onRequest\")", t);
        engine_free_string(res);
    }

    // graph query benchmark
    {
        t0 = Clock::now();
        char *res = engine_graph_query(pid, "MATCH (Function)->(Function)");
        double t = elapsed_ms(t0);
        printf("%-35s %10.1f ms\n", "graphQuery(Function->Function)", t);
        engine_free_string(res);
    }

    engine_shutdown();
    printf("\n=== Benchmark complete ===\n");
    return 0;
}
