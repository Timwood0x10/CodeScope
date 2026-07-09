#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Test fixture paths — kept as constants so cleanup is reliable.
static const char *kProjDir = "/tmp/test_enhance_e2e";
static const char *kDbPath = "/tmp/test_enhance_e2e.db";

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "\nFAIL: %s\n", msg);
		// Best-effort cleanup before exiting so re-runs aren't poisoned.
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
		fs::remove(kDbPath, ec);
		exit(1);
	}
}

static void check_json_has(const char *json, const char *key, const char *msg)
{
	if (strstr(json, key) == nullptr) {
		fprintf(stderr, "\nFAIL: %s — missing '%s' in:\n%s\n", msg, key,
			json);
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
		fs::remove(kDbPath, ec);
		exit(1);
	}
}

// Write a source file with content under proj_dir.
static void write_file(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, ("write_file: cannot open " + path).c_str());
	fputs(content, f);
	fclose(f);
}

int main()
{
	std::error_code ec;
	fs::remove_all(kProjDir, ec);
	fs::create_directories(kProjDir);
	fs::remove(kDbPath, ec);

	// main.cpp — calls helper (cross-file)
	write_file(std::string(kProjDir) + "/main.cpp", R"(#include "helper.h"
int main() {
    helper(42);
    return 0;
}
)");

	// helper.h — declaration
	write_file(std::string(kProjDir) + "/helper.h", R"(#pragma once
int helper(int x);
int helper(double x);
)");

	// helper.cpp — two overloaded helpers + internal helper
	write_file(std::string(kProjDir) + "/helper.cpp", R"(#include "helper.h"
#include <cstdio>

int helper(int x) {
    return internal_impl(x) + 1;
}

int helper(double x) {
    return static_cast<int>(x);
}

static int internal_impl(int x) {
    return x * 2;
}
)");

	// ─── Step 1: init + create project ───
	check(engine_init(kDbPath) == 0, "engine_init");
	uint64_t pid = engine_create_project(kProjDir, "enhance_e2e");
	check(pid > 0, "create_project");
	printf("PASS: project_id=%llu\n", (unsigned long long)pid);

	// ─── Step 2: index (3 files) ───
	char *idx = engine_index_project(pid, kProjDir, NULL);
	check(idx != nullptr, "index_project result");
	check_json_has(idx, "\"ok\":true", "index_project ok");
	printf("PASS: index ok — %s\n", idx);
	engine_free_string(idx);

	// ─── Step 3: enhance (first run) ───
	char *enh = engine_enhance_project(pid);
	check(enh != nullptr, "enhance_project result");
	check_json_has(enh, "\"files_processed\"",
		       "enhance: has files_processed");
	int files1 = 0, edges1 = 0, syms1 = 0;
	sscanf(enh,
	       "{\"files_processed\":%d,\"symbols_enhanced\":%d,\"call_edges\":%d",
	       &files1, &syms1, &edges1);
	printf("PASS: enhance run1 — files=%d symbols=%d edges=%d\n%s\n",
	       files1, syms1, edges1, enh);
	engine_free_string(enh);

	// Verify we processed files and found edges
	check(files1 > 0, "enhance: files_processed > 0");
	check(edges1 > 0, "enhance: call_edges > 0");
	check(syms1 > 0, "enhance: symbols_enhanced > 0");

	// ─── Step 4: check enhancement status ───
	char *st = engine_get_enhancement_status(pid);
	check(st != nullptr, "enhancement_status result");
	int total_st = 0, cg_st = 0, met_st = 0, emb_st = 0;
	sscanf(st,
	       "{\"total_symbols\":%d,\"callgraph_ready\":%d,\"metrics_ready\":%d,\"embedding_ready\":%d",
	       &total_st, &cg_st, &met_st, &emb_st);
	printf("PASS: status — total=%d cg=%d metrics=%d emb=%d\n%s\n",
	       total_st, cg_st, met_st, emb_st, st);
	engine_free_string(st);

	// At least some symbols should be fully enhanced.
	// metrics_ready should equal callgraph_ready because both are set by
	// markCallgraphAndMetricsReady. embedding_ready may be lower if vec0
	// is unavailable on this platform — assert callgraph/metrics only.
	check(cg_st > 0, "status: callgraph_ready > 0");
	check(met_st > 0, "status: metrics_ready > 0");

	// ─── Step 5: cross-file caller/callee checks ───
	// main() calls helper(int) via `helper(42)` — cross-file edge from buildGraph
	char *callees_main = engine_get_callees(pid, "main");
	check(callees_main != nullptr, "callees of main");
	check_json_has(callees_main, "helper", "callees of main: has helper");
	printf("PASS: callees of main (cross-file) ok\n%s\n", callees_main);
	engine_free_string(callees_main);

	// helper(int) calls internal_impl — same-file regex edge
	char *callees_helper = engine_get_callees(pid, "helper");
	check(callees_helper != nullptr, "callees of helper(int)");
	check_json_has(callees_helper, "internal_impl",
		       "callees of helper: has internal_impl");
	printf("PASS: callees of helper (same-file) ok\n%s\n", callees_helper);
	engine_free_string(callees_helper);

	// internal_impl is called by helper(int)
	char *callers_impl = engine_get_callers(pid, "internal_impl");
	check(callers_impl != nullptr, "callers of internal_impl");
	check_json_has(callers_impl, "helper",
		       "callers of internal_impl: has helper");
	printf("PASS: callers of internal_impl ok\n%s\n", callers_impl);
	engine_free_string(callers_impl);

	// ─── Step 6: rerun idempotency ───
	char *enh2 = engine_enhance_project(pid);
	check(enh2 != nullptr, "enhance_project rerun");
	int files2 = -1, edges2 = -1, syms2 = -1;
	sscanf(enh2,
	       "{\"files_processed\":%d,\"symbols_enhanced\":%d,\"call_edges\":%d",
	       &files2, &syms2, &edges2);
	char *st2 = engine_get_enhancement_status(pid);
	check(st2 != nullptr, "status after rerun");
	int cg_st2 = 0;
	sscanf(st2, "{\"total_symbols\":%d,\"callgraph_ready\":%d", &total_st,
	       &cg_st2);
	// Idempotency: callgraph_ready must not decrease, and the rerun must
	// not insert duplicate edges. `call_edges` in the JSON counts NEW edges
	// inserted during this run — on a true idempotent rerun it should be 0
	// because all edges already exist and are deduped by INSERT OR IGNORE.
	check(cg_st2 == cg_st, "rerun: callgraph_ready unchanged");
	check(edges2 == 0,
	      "rerun: no new call_edges (all deduped by unique index)");
	printf("PASS: rerun idempotent — new edges %d→%d / cg_ready %d→%d\n%s\n%s\n",
	       edges1, edges2, cg_st, cg_st2, enh2, st2);
	engine_free_string(enh2);
	engine_free_string(st2);

	// ─── Step 7: trace path (BFS on call_edges) ───
	char *trace = engine_trace_path(pid, "main", "internal_impl");
	check(trace != nullptr, "trace_path result");
	check_json_has(trace, "\"path\"", "trace: has path");
	check_json_has(trace, "main", "trace: contains main");
	check_json_has(trace, "helper", "trace: contains helper");
	check_json_has(trace, "internal_impl", "trace: contains internal_impl");
	printf("PASS: trace_path main→internal_impl ok\n%s\n", trace);
	engine_free_string(trace);

	// ─── Step 8: overview (smoke check) ───
	char *overview = engine_get_project_overview(pid);
	check(overview != nullptr, "overview");
	check_json_has(overview, "\"total_nodes\"",
		       "overview: has total_nodes");
	printf("PASS: overview ok\n%s\n", overview);
	engine_free_string(overview);

	// ─── Cleanup ───
	engine_shutdown();
	fs::remove_all(kProjDir, ec);
	fs::remove(kDbPath, ec);

	printf("\n=== ENHANCE E2E TEST PASSED ===\n");
	return 0;
}
