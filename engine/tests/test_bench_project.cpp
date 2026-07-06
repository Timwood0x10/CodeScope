/**
 * Benchmark: analyze a real-world project.
 * Measures wall-clock time, peak RSS, CPU utilization, and per-phase breakdown.
 *
 * Usage:
 *   ./test_bench_project <grammars_dir> <project_dir> [max_file_size_mb] [lang_filter]
 *
 * JSON output (machine-readable):
 *   CODESCOPE_BENCH_JSON=<path> ./test_bench_project ...
 */
#include "../include/engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/utsname.h>
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

static void jsonEscape(std::ostream &os, const char *s) {
    if (!s) { os << "\"\""; return; }
    os << '"';
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"': os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default: os << *p;
        }
    }
    os << '"';
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <grammars_dir> <project_dir> [max_file_size_mb] [lang_filter]\n", argv[0]);
        fprintf(stderr, "  CODESCOPE_BENCH_JSON=<path>  Write machine-readable JSON report\n");
        return 1;
    }
    setenv("GRAMMARS_DIR", argv[1], 1);
    setenv("CODESCOPE_VERBOSE", "0", 1);

    int max_mb = (argc > 3) ? atoi(argv[3]) : 1;
    if (max_mb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", max_mb * 1024 * 1024);
        setenv("CODESCOPE_MAX_FILE_SIZE", buf, 1);
    }

    const char *lang_filter = (argc > 4) ? argv[4] : "typescript,tsx,javascript";
    const char *json_out = getenv("CODESCOPE_BENCH_JSON");
    bool json_mode = json_out && *json_out;

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

    // Phase 0: count source files
    std::string count_cmd = std::string("find ") + argv[2] + " " + ext_filter +
        " 2>/dev/null | grep -v node_modules | wc -l";
    FILE *fp = popen(count_cmd.c_str(), "r");
    char count_buf[64] = {0};
    if (fp) { fgets(count_buf, sizeof(count_buf), fp); pclose(fp); }
    int total_files = atoi(count_buf);

    std::string size_cmd = std::string("find ") + argv[2] + " " + ext_filter +
        " 2>/dev/null | grep -v node_modules | xargs wc -l 2>/dev/null | tail -1";
    fp = popen(size_cmd.c_str(), "r");
    char size_buf[64] = {0};
    if (fp) { fgets(size_buf, sizeof(size_buf), fp); pclose(fp); }
    double total_lines = atof(size_buf);

    // Get git rev
    std::string git_rev;
    {
        FILE *gf = popen("cd /Users/scc/code/cppCode/CodeScope && git rev-parse --short HEAD 2>/dev/null", "r");
        char gb[64] = {0};
        if (gf) { fgets(gb, sizeof(gb), gf); pclose(gf); }
        size_t len = strlen(gb);
        if (len > 0 && gb[len-1] == '\n') gb[len-1] = '\0';
        git_rev = gb;
    }

    // Get machine info
    struct utsname uname_data;
    std::string machine = "unknown";
    if (uname(&uname_data) == 0) {
        machine = std::string(uname_data.sysname) + " " + uname_data.release;
    }

    // Timestamp
    auto now = std::time(nullptr);
    char ts_buf[64];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S%z", std::localtime(&now));

    unlink("/tmp/bench_project.db");
    unlink("/tmp/bench_project.db-wal");
    unlink("/tmp/bench_project.db-shm");

    size_t rss_before = peak_rss_kb();
    double cpu_before = cpu_seconds();

    auto t0 = Clock::now();
    int rc = engine_init("/tmp/bench_project.db");
    double init_ms = elapsed_ms(t0);
    if (rc != 0) { fprintf(stderr, "FAIL: engine_init\n"); return 1; }

    t0 = Clock::now();
    uint64_t pid = engine_create_project(argv[2], "bench-project");
    double create_ms = elapsed_ms(t0);

    // Index
    auto idx_start = Clock::now();
    char *result = engine_index_project(pid, argv[2], lang_filter);
    double index_ms = elapsed_ms(idx_start);

    size_t rss_after = peak_rss_kb();
    double cpu_after = cpu_seconds();

    int files_idx = 0;
    if (strstr(result, "\"files_indexed\":"))
        sscanf(strstr(result, "\"files_indexed\":"), "\"files_indexed\":%d", &files_idx);
    if (total_files == 0) total_files = files_idx;

    int64_t time_parse_ms = extract_int64(result, "time_parse_ms");
    int64_t time_sqlite_ms = extract_int64(result, "time_sqlite_ms");
    int64_t time_buildgraph_ms = extract_int64(result, "time_buildgraph_ms");
    int64_t time_ftsvector_ms = extract_int64(result, "time_ftsvector_ms");

    // Save copy for human-readable output before freeing
    std::string result_str = result ? result : "";
    engine_free_string(result);

    // Query benchmarks
    double q_search_code = 0, q_search_semantic = 0;
    double q_callers = 0, q_callees = 0, q_graph = 0;

    {
        t0 = Clock::now(); char *r = engine_search_code(pid, "function", 10);
        q_search_code = elapsed_ms(t0); engine_free_string(r);
    }
    {
        t0 = Clock::now(); char *r = engine_search_semantic(pid, "function", 10);
        q_search_semantic = elapsed_ms(t0); engine_free_string(r);
    }
    {
        t0 = Clock::now(); char *r = engine_get_callers(pid, "onRequest");
        q_callers = elapsed_ms(t0); engine_free_string(r);
    }
    {
        t0 = Clock::now(); char *r = engine_get_callees(pid, "onRequest");
        q_callees = elapsed_ms(t0); engine_free_string(r);
    }
    {
        t0 = Clock::now(); char *r = engine_graph_query(pid, "MATCH (Function)->(Function)");
        q_graph = elapsed_ms(t0); engine_free_string(r);
    }

    // Graph stats
    char *stats = engine_get_graph_stats(pid);
    int64_t total_nodes = extract_int64(stats, "total_nodes");
    int64_t total_edges = extract_int64(stats, "total_edges");
    engine_free_string(stats);

    engine_shutdown();

    // ── JSON report ───────────────────────────────────────────────
    if (json_mode) {
        std::ofstream jf(json_out);
        if (!jf.is_open()) {
            fprintf(stderr, "FAIL: cannot write %s\n", json_out);
            return 1;
        }
        jf << "{\n";
        jf << "  \"schema_version\": 1,\n";
        jf << "  \"project\": "; jsonEscape(jf, argv[2]); jf << ",\n";
        jf << "  \"lang_filter\": "; jsonEscape(jf, lang_filter); jf << ",\n";
        jf << "  \"git_rev\": "; jsonEscape(jf, git_rev.c_str()); jf << ",\n";
        jf << "  \"timestamp\": "; jsonEscape(jf, ts_buf); jf << ",\n";
        jf << "  \"machine\": "; jsonEscape(jf, machine.c_str()); jf << ",\n";
        jf << "  \"config\": { \"max_file_size_mb\": " << max_mb
           << ", \"grammars_dir\": "; jsonEscape(jf, argv[1]); jf << " },\n";
        jf << "  \"files\": " << total_files << ",\n";
        jf << "  \"files_indexed\": " << files_idx << ",\n";
        jf << "  \"lines\": " << static_cast<int64_t>(total_lines) << ",\n";
        jf << "  \"nodes\": " << total_nodes << ",\n";
        jf << "  \"edges\": " << total_edges << ",\n";
        jf << "  \"rss_kb\": " << rss_after << ",\n";
        jf << "  \"rss_delta_kb\": " << (rss_after - rss_before) << ",\n";
        jf << "  \"cpu_seconds\": " << (cpu_after - cpu_before) << ",\n";
        jf << "  \"phase_ms\": {\n";
        jf << "    \"parse\": " << time_parse_ms << ",\n";
        jf << "    \"sqlite\": " << time_sqlite_ms << ",\n";
        jf << "    \"buildgraph\": " << time_buildgraph_ms << ",\n";
        jf << "    \"ftsvector\": " << time_ftsvector_ms << ",\n";
        jf << "    \"total\": " << static_cast<int64_t>(index_ms) << "\n";
        jf << "  },\n";
        jf << "  \"query_ms\": {\n";
        jf << "    \"search_code\": " << q_search_code << ",\n";
        jf << "    \"search_semantic\": " << q_search_semantic << ",\n";
        jf << "    \"callers\": " << q_callers << ",\n";
        jf << "    \"callees\": " << q_callees << ",\n";
        jf << "    \"graph_query\": " << q_graph << "\n";
        jf << "  }\n";
        jf << "}\n";
        jf.close();
        printf("JSON report written to %s\n", json_out);
        return 0;
    }

    // ── Human-readable output ─────────────────────────────────────
    printf("max_file_size: %d MB\n", max_mb);
    printf("lang_filter:     %s\n", lang_filter);
    printf("project_dir:     %s\n", argv[2]);
    printf("total_source_files: %d\n", total_files);
    printf("total_lines:     %.0f total\n", total_lines);

    printf("\nengine_init: %d (%.0f ms)\n", rc, init_ms);
    printf("create_project: %llu (%.0f ms)\n", (unsigned long long)pid, create_ms);

    printf("\n--- INDEXING ---\n");
    printf("index_time:      %.0f ms (%.2f s)\n", index_ms, index_ms / 1000.0);
    printf("result:          %s\n", result_str.c_str());

    int64_t time_accounted = time_parse_ms + time_sqlite_ms + time_buildgraph_ms + time_ftsvector_ms;
    int64_t time_unaccounted = static_cast<int64_t>(index_ms) - time_accounted;

    printf("\n--- PHASE BREAKDOWN ---\n");
    if (time_accounted > 0) {
        printf("%-20s %10s %8s\n", "Phase", "Time (ms)", "Percent");
        printf("%-20s %10s %8s\n", "--------------------", "----------", "--------");
        printf("%-20s %10lld %7.1f%%\n", "Parse (parallel)", (long long)time_parse_ms, 100.0 * time_parse_ms / index_ms);
        printf("%-20s %10lld %7.1f%%\n", "SQLite (serial write)", (long long)time_sqlite_ms, 100.0 * time_sqlite_ms / index_ms);
        printf("%-20s %10lld %7.1f%%\n", "buildGraph (post)", (long long)time_buildgraph_ms, 100.0 * time_buildgraph_ms / index_ms);
        printf("%-20s %10lld %7.1f%%\n", "FTS/vectors (post)", (long long)time_ftsvector_ms, 100.0 * time_ftsvector_ms / index_ms);
        if (time_unaccounted > 0)
            printf("%-20s %10lld %7.1f%%\n", "(overhead/other)", (long long)time_unaccounted, 100.0 * time_unaccounted / index_ms);
        printf("%-20s %10s %8s\n", "--------------------", "----------", "--------");
        printf("%-20s %10lld %7.1f%%\n", "Total", (long long)index_ms, 100.0);
    }

    printf("\n--- GRAPH STATS ---\n");
    printf("stats: nodes=%lld edges=%lld files=%lld (%.0f ms)\n",
           (long long)total_nodes, (long long)total_edges, (long long)files_idx);

    printf("\n=== PERFORMANCE SUMMARY ===\n");
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
        if (total_lines > 0)
            printf("%-25s %.0f lines/sec\n", "throughput (lines)", total_lines / (index_ms / 1000.0));
    }
    printf("\n=== QUERY BENCHMARK ===\n");
    printf("%-35s %10.1f ms\n", "searchCode(\"function\",10)", q_search_code);
    printf("%-35s %10.1f ms\n", "searchSemantic(\"function\",10)", q_search_semantic);
    printf("%-35s %10.1f ms\n", "getCallers(\"onRequest\")", q_callers);
    printf("%-35s %10.1f ms\n", "getCallees(\"onRequest\")", q_callees);
    printf("%-35s %10.1f ms\n", "graphQuery(Function->Function)", q_graph);
    printf("\n=== Benchmark complete ===\n");
    return 0;
}
