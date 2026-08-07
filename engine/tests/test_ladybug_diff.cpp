// test_ladybug_diff.cpp
//
// LadybugDB correctness test: verify that the migrated graph queries return
// correct results when run exclusively on the LadybugDB path. After the
// LadybugDB-only migration there is no SQLite fallback — setting the test
// hook (engine_set_ladybug_queries_enabled) to 0 makes queries return an
// error, so we keep it at the default (enabled) and check results against
// EXPECTED values derived from the known call graph of the test project
// (not against a second code path).
//
// Flow:
//   1. Create a small multi-file test project with a known call graph.
//   2. engine_index_project → buildGraph → compileGraphToLadybugDB (sync).
//   3. For each query: run via the LadybugDB path, assert non-null and
//      not an error, and assert the result contains the expected node
//      names.

#include "../include/engine.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

// Extract the set of "name":"..." values from a JSON string. Used to verify
// that a query result contains the expected node names regardless of field
// ordering or extra fields.
static std::set<std::string> extractNames(const char *json)
{
	std::set<std::string> out;
	if (!json)
		return out;
	std::string s(json);
	const std::string key = "\"name\":\"";
	size_t pos = 0;
	while ((pos = s.find(key, pos)) != std::string::npos) {
		pos += key.size();
		std::string val;
		while (pos < s.size()) {
			char c = s[pos++];
			if (c == '\\' && pos < s.size()) {
				char n = s[pos++];
				if (n == 'n')
					val += '\n';
				else if (n == 't')
					val += '\t';
				else if (n == 'r')
					val += '\r';
				else
					val += n; // \" -> ", \\ -> \
				continue;
			}
			if (c == '"')
				break;
			val += c;
		}
		if (!val.empty())
			out.insert(val);
	}
	return out;
}

// Run `call_expr` on the LadybugDB path and assert:
//   - result is non-null
//   - result does not contain an "error" field (or contains "error":null)
//   - result contains every name in `expected_names`
// If `expected_names` is empty, only the non-null and non-error checks
// are performed (used for queries whose result schema is not a list of
// named nodes — those get additional manual assertions).
#define VERIFY_CHECK(label, call_expr, expected_names)                        \
	do {                                                                  \
		char *result = (call_expr);                                   \
		check(result != nullptr, label " (null result)");             \
		if (result) {                                                 \
			check(strstr(result, "\"error\"") == nullptr ||       \
				      strstr(result, "\"error\":null") !=     \
					      nullptr,                        \
			      label " returned error");                       \
			if ((expected_names).size() > 0) {                    \
				auto got = extractNames(result);              \
				for (const auto &n : (expected_names)) {      \
					std::string m =                       \
						std::string(label) +          \
						": missing expected name '" + \
						n + "'";                      \
					check(got.count(n) > 0, m.c_str());   \
				}                                             \
			}                                                     \
			engine_free_string(result);                           \
			++passed;                                             \
			fprintf(stderr, "  [PASS] %s\n", label);              \
		}                                                             \
	} while (0)

int main()
{
	// ── Create a small multi-file test project with a known call graph ──
	const char *proj_dir = "/tmp/test_ladybug_diff";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	// math.go: add is called by multiply and compute.
	{
		FILE *f = fopen((std::string(proj_dir) + "/math.go").c_str(),
				"w");
		check(f != nullptr, "fopen math.go");
		fputs("package main\n\n"
		      "func add(a, b int) int { return a + b }\n"
		      "func multiply(a, b int) int {\n"
		      "    result := 0\n"
		      "    for i := 0; i < b; i++ {\n"
		      "        result = add(result, a)\n"
		      "    }\n"
		      "    return result\n"
		      "}\n"
		      "func compute(x, y int) int {\n"
		      "    return multiply(add(x, y), add(x, y))\n"
		      "}\n",
		      f);
		fclose(f);
	}

	// main.go: main calls compute.
	{
		FILE *f = fopen((std::string(proj_dir) + "/main.go").c_str(),
				"w");
		check(f != nullptr, "fopen main.go");
		fputs("package main\n\n"
		      "func main() {\n"
		      "    result := compute(3, 4)\n"
		      "    println(result)\n"
		      "}\n",
		      f);
		fclose(f);
	}

	// ── Init engine and index ──────────────────────────────────
	char db_path[] = "/tmp/test_ladybug_diff.db";
	unlink(db_path);
	unlink("/tmp/test_ladybug_diff.lbug");
	// Kuzu uses DOT-separated companions (.lbug.wal/.lbug.shm); stale
	// ones from an aborted prior run make lbug_database_init fail on
	// open. Clean them here so the test is hermetic.
	unlink("/tmp/test_ladybug_diff.lbug.wal");
	unlink("/tmp/test_ladybug_diff.lbug.shm");

	check(engine_init(db_path) == 0, "engine_init");

	uint64_t pid = engine_create_project(proj_dir, "ladybug-diff-test");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	// buildGraph compiles LadybugDB synchronously, so isGraphReady() is
	// true by now; a short settle delay guards any trailing async work.
	usleep(200000);

	// After the migration, LadybugDB-first routing is always on; set the
	// hook to 1 for clarity (this is also the engine default).
	engine_set_ladybug_queries_enabled(1);

	int passed = 0, total = 0;

	// Expected relationships (from the project above):
	//   add     ← multiply, compute      (callers of add)
	//   main    → compute                (callees of main)
	//   compute → multiply, add          (callees of compute)
	//   multiply → add                   (callees of multiply)

	// ── Caller / callee / reference checks ──
	total++;
	{
		std::vector<std::string> expected = { "multiply", "compute" };
		VERIFY_CHECK("getCallers(add)",
			     engine_get_callers(pid, "add", nullptr), expected);
	}

	total++;
	{
		std::vector<std::string> expected = { "compute" };
		VERIFY_CHECK("getCallees(main)",
			     engine_get_callees(pid, "main", nullptr),
			     expected);
	}

	total++;
	{
		std::vector<std::string> expected = { "multiply", "add" };
		VERIFY_CHECK("getCallees(compute)",
			     engine_get_callees(pid, "compute", nullptr),
			     expected);
	}

	total++;
	{
		std::vector<std::string> expected = { "add" };
		VERIFY_CHECK("getCallees(multiply)",
			     engine_get_callees(pid, "multiply", nullptr),
			     expected);
	}

	total++;
	{
		std::vector<std::string> expected = { "multiply", "compute" };
		VERIFY_CHECK("findReferences(add)",
			     engine_find_references(pid, "add", nullptr),
			     expected);
	}

	// ── getHotspots: project has callers, so the list must be non-empty.
	// "add" is the most-called function (3 call sites) and must appear.
	total++;
	{
		std::vector<std::string> expected = { "add" };
		VERIFY_CHECK("getHotspots", engine_get_hotspots(pid, 10),
			     expected);
	}

	// ── getEntryPoints: "main" is the entry point of the project.
	total++;
	{
		std::vector<std::string> expected = { "main" };
		VERIFY_CHECK("getEntryPoints", engine_get_entry_points(pid),
			     expected);
	}

	// ── detect_changes: must succeed (error:null) and find modified
	// nodes for math.go (add/multiply/compute). ──
	total++;
	{
		char *dc = engine_detect_changes(
			pid, "[\"/tmp/test_ladybug_diff/math.go\"]");
		check(dc != nullptr, "detect_changes null");
		check(strstr(dc, "\"error\":null") != nullptr,
		      "detect_changes error:null");
		auto got = extractNames(dc);
		check(!got.empty(), "detect_changes returned no nodes");
		engine_free_string(dc);
		++passed;
		fprintf(stderr, "  [PASS] detect_changes\n");
	}

	// ── traceCallChain: must find a path main → ... → add ──
	total++;
	{
		char *tc = engine_trace_call_chain(pid, "main", "add");
		check(tc != nullptr, "traceCallChain null");
		check(strstr(tc, "\"found\":true") != nullptr,
		      "traceCallChain found:true");
		check(strstr(tc, "main") != nullptr &&
			      strstr(tc, "add") != nullptr,
		      "traceCallChain endpoints");
		engine_free_string(tc);
		++passed;
		fprintf(stderr, "  [PASS] traceCallChain\n");
	}

	// ── findDefinition: result must contain "add" ──
	total++;
	{
		char *def = engine_find_definition(pid, "add", nullptr);
		check(def != nullptr, "findDefinition null");
		check(strstr(def, "add") != nullptr,
		      "findDefinition contains add");
		engine_free_string(def);
		++passed;
		fprintf(stderr, "  [PASS] findDefinition\n");
	}

	// ── getGraphStats: must report total_nodes > 0 and total_edges > 0.
	// The project has 4 functions (add, multiply, compute, main) and 4+
	// call edges. ──
	total++;
	{
		char *stats = engine_get_graph_stats(pid);
		check(stats != nullptr, "getGraphStats null");
		const char *nodes = strstr(stats, "\"total_nodes\":");
		const char *edges = strstr(stats, "\"total_edges\":");
		check(nodes != nullptr, "getGraphStats has total_nodes");
		check(edges != nullptr, "getGraphStats has total_edges");
		// Verify the integer value following each key is non-zero.
		if (nodes) {
			nodes += strlen("\"total_nodes\":");
			while (*nodes == ' ' || *nodes == '\t')
				nodes++;
			check(*nodes != '0', "getGraphStats total_nodes > 0");
		}
		if (edges) {
			edges += strlen("\"total_edges\":");
			while (*edges == ' ' || *edges == '\t')
				edges++;
			check(*edges != '0', "getGraphStats total_edges > 0");
		}
		engine_free_string(stats);
		++passed;
		fprintf(stderr, "  [PASS] getGraphStats\n");
	}

	// ── Summary ──
	fprintf(stderr, "\n=== LadybugDB correctness test: %d/%d passed ===\n",
		passed, total);

	engine_shutdown();
	std::filesystem::remove_all(proj_dir);
	unlink(db_path);
	unlink("/tmp/test_ladybug_diff.lbug");
	// Kuzu uses DOT-separated companions (.lbug.wal/.lbug.shm); stale
	// ones from an aborted prior run make lbug_database_init fail on
	// open. Clean them here so the test is hermetic.
	unlink("/tmp/test_ladybug_diff.lbug.wal");
	unlink("/tmp/test_ladybug_diff.lbug.shm");

	return passed == total ? 0 : 1;
}
