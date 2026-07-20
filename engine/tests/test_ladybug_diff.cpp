// test_ladybug_diff.cpp
//
// Differential test: compare LadybugDB query results against the SQLite
// fallback for every migrated graph query. The engine exposes a test hook
// (engine_set_ladybug_queries_enabled) that disables LadybugDB-first routing,
// forcing the SQLite path. We run each query on BOTH paths and assert the
// two produce the same set of node names — preventing silent divergence
// between the graph engine and the SQLite fallback.
//
// Flow:
//   1. Create a small multi-file test project with a known call graph.
//   2. engine_index_project → buildGraph → compileGraphToLadybugDB (sync).
//   3. For each query: run via LadybugDB path, then via SQLite path, compare.
//   4. Assert: name sets match AND expected relationships hold.

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

// Extract the set of "name":"..." values from a JSON string. Used to compare
// the two query paths at the level of "which nodes were returned" rather than
// exact byte layout (field order/extra fields may differ between paths).
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

static bool sameNames(const char *a, const char *b)
{
	return extractNames(a) == extractNames(b);
}

// Extract all "<field>":<number> values from a JSON string as a multiset.
// Used to compare numeric field values (e.g. complexity, nesting) between
// the two query paths at the value level, not just the name-set level.
// A multiset is used (not a set) so that duplicate values are preserved
// (e.g. two nodes with complexity 1 each contribute two entries).
static std::multiset<int> extractFieldValues(const char *json,
					     const char *field)
{
	std::multiset<int> out;
	if (!json || !field || !*field)
		return out;
	std::string s(json);
	std::string key = std::string("\"") + field + "\":";
	size_t pos = 0;
	while ((pos = s.find(key, pos)) != std::string::npos) {
		pos += key.size();
		// Skip whitespace between key and value.
		while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
			pos++;
		if (pos >= s.size())
			break;
		// Parse optional sign + digits.
		int sign = 1;
		if (s[pos] == '-') {
			sign = -1;
			pos++;
		}
		if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
			int val = 0;
			while (pos < s.size() && s[pos] >= '0' &&
			       s[pos] <= '9') {
				val = val * 10 + (s[pos] - '0');
				pos++;
			}
			out.insert(sign * val);
		}
	}
	return out;
}

// Run `call_expr` on both query paths and assert the returned name sets match.
// Also asserts the Ladybug result is non-empty when expect_nonempty is set,
// which proves LadybugDB was actually exercised (not a vacuous pass).
#define DIFF_CHECK(label, call_expr, expect_nonempty)                       \
	do {                                                                   \
		engine_set_ladybug_queries_enabled(1);                          \
		char *lbug = (call_expr);                                       \
		check(lbug != nullptr, label " (ladybug null)");                \
		engine_set_ladybug_queries_enabled(0);                          \
		char *sqlite = (call_expr);                                      \
		check(sqlite != nullptr, label " (sqlite null)");              \
		engine_set_ladybug_queries_enabled(1);                          \
		if (expect_nonempty)                                            \
			check(!extractNames(lbug).empty(),                      \
			      label " (ladybug returned no nodes)");           \
		bool match = sameNames(lbug, sqlite);                           \
		if (!match) {                                                    \
			fprintf(stderr, "  [DIFF] %s\n", label);                \
			fprintf(stderr, "    ladybug: %s\n", lbug);             \
			fprintf(stderr, "    sqlite : %s\n", sqlite);            \
		}                                                                \
		check(match, label " diverges between LadybugDB and SQLite");   \
		engine_free_string(lbug);                                       \
		engine_free_string(sqlite);                                     \
		++passed;                                                       \
		fprintf(stderr, "  [PASS] %s\n", label);                       \
	} while (0)

// Assert the Ladybug result contains every expected node name.
static void expectContains(const char *label, const char *json,
			   const std::vector<std::string> &names)
{
	auto got = extractNames(json);
	for (const auto &n : names) {
		std::string m = std::string(label) +
				": missing expected name '" + n + "'";
		check(got.count(n) > 0, m.c_str());
	}
}

int main()
{
	// ── Create a small multi-file test project with a known call graph ──
	const char *proj_dir = "/tmp/test_ladybug_diff";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	// math.go: add is called by multiply and compute.
	{
		FILE *f = fopen((std::string(proj_dir) + "/math.go").c_str(), "w");
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
		FILE *f = fopen((std::string(proj_dir) + "/main.go").c_str(), "w");
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

	check(engine_init(db_path) == 0, "engine_init");

	uint64_t pid = engine_create_project(proj_dir, "ladybug-diff-test");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	// buildGraph compiles LadybugDB synchronously, so isGraphReady() is true
	// by now; a short settle delay guards any trailing async work.
	usleep(200000);

	int passed = 0, total = 0;

	// ── Differential checks: LadybugDB path vs SQLite fallback ──
	// Expected relationships (from the project above):
	//   add    ← multiply, compute      (callers of add)
	//   main   → compute                (callees of main)
	//   compute → multiply, add         (callees of compute)
	//   multiply → add                  (callees of multiply)

	total++;
	DIFF_CHECK("getCallers(add)",
		   engine_get_callers(pid, "add", nullptr), true);
	expectContains("getCallers(add)",
		       engine_get_callers(pid, "add", nullptr),
		       {"multiply", "compute"});

	total++;
	DIFF_CHECK("getCallees(main)",
		   engine_get_callees(pid, "main", nullptr), true);
	expectContains("getCallees(main)",
		       engine_get_callees(pid, "main", nullptr),
		       {"compute"});

	total++;
	DIFF_CHECK("getCallees(compute)",
		   engine_get_callees(pid, "compute", nullptr), true);
	expectContains("getCallees(compute)",
		       engine_get_callees(pid, "compute", nullptr),
		       {"multiply", "add"});

	total++;
	DIFF_CHECK("getCallees(multiply)",
		   engine_get_callees(pid, "multiply", nullptr), true);
	expectContains("getCallees(multiply)",
		       engine_get_callees(pid, "multiply", nullptr),
		       {"add"});

	total++;
	DIFF_CHECK("findReferences(add)",
		   engine_find_references(pid, "add", nullptr), true);
	expectContains("findReferences(add)",
		       engine_find_references(pid, "add", nullptr),
		       {"multiply", "compute"});

	// ── getHotspots: both paths agree on name sets ──
	total++;
	DIFF_CHECK("getHotspots",
		   engine_get_hotspots(pid, 10), true);

	// Value-consistency check: complexity values must match between the
	// two paths (not just the name sets). Bug M2: LadybugDB path used
	// to emit complexity:0 for all nodes.
	{
		total++;
		engine_set_ladybug_queries_enabled(1);
		char *lbug_hs = engine_get_hotspots(pid, 10);
		engine_set_ladybug_queries_enabled(0);
		char *sqlite_hs = engine_get_hotspots(pid, 10);
		engine_set_ladybug_queries_enabled(1);
		check(lbug_hs != nullptr && sqlite_hs != nullptr,
		      "getHotspots value-check null");
		auto lbug_cx = extractFieldValues(lbug_hs, "complexity");
		auto sqlite_cx =
			extractFieldValues(sqlite_hs, "complexity");
		if (lbug_cx != sqlite_cx) {
			fprintf(stderr,
				"  [DIFF] getHotspots complexity\n");
			fprintf(stderr, "    ladybug: %s\n", lbug_hs);
			fprintf(stderr, "    sqlite : %s\n", sqlite_hs);
		}
		check(lbug_cx == sqlite_cx,
		      "getHotspots complexity diverges between LadybugDB "
		      "and SQLite");
		engine_free_string(lbug_hs);
		engine_free_string(sqlite_hs);
		++passed;
		fprintf(stderr, "  [PASS] getHotspots complexity\n");
	}

	// ── getEntryPoints: both paths agree on name sets ──
	total++;
	DIFF_CHECK("getEntryPoints",
		   engine_get_entry_points(pid), true);

	// Value-consistency check: complexity and nesting values must match
	// between the two paths. Bug M2: LadybugDB path used to emit
	// complexity:0 and nesting:0 for all nodes.
	{
		total++;
		engine_set_ladybug_queries_enabled(1);
		char *lbug_ep = engine_get_entry_points(pid);
		engine_set_ladybug_queries_enabled(0);
		char *sqlite_ep = engine_get_entry_points(pid);
		engine_set_ladybug_queries_enabled(1);
		check(lbug_ep != nullptr && sqlite_ep != nullptr,
		      "getEntryPoints value-check null");
		auto lbug_cx = extractFieldValues(lbug_ep, "complexity");
		auto sqlite_cx =
			extractFieldValues(sqlite_ep, "complexity");
		auto lbug_nest = extractFieldValues(lbug_ep, "nesting");
		auto sqlite_nest =
			extractFieldValues(sqlite_ep, "nesting");
		if (lbug_cx != sqlite_cx || lbug_nest != sqlite_nest) {
			fprintf(stderr,
				"  [DIFF] getEntryPoints values\n");
			fprintf(stderr, "    ladybug: %s\n", lbug_ep);
			fprintf(stderr, "    sqlite : %s\n", sqlite_ep);
		}
		check(lbug_cx == sqlite_cx,
		      "getEntryPoints complexity diverges");
		check(lbug_nest == sqlite_nest,
		      "getEntryPoints nesting diverges");
		engine_free_string(lbug_ep);
		engine_free_string(sqlite_ep);
		++passed;
		fprintf(stderr, "  [PASS] getEntryPoints values\n");
	}

	// ── analyzeChangeImpact (detect_changes): both paths agree ──
	// Coverage for M3: ensures the LadybugDB adjacency path produces the
	// same caller/callee name sets as the SQLite fallback.
	total++;
	{
		engine_set_ladybug_queries_enabled(1);
		char *lbug = engine_detect_changes(
			pid, "[\"/tmp/test_ladybug_diff/math.go\"]");
		engine_set_ladybug_queries_enabled(0);
		char *sqlite = engine_detect_changes(
			pid, "[\"/tmp/test_ladybug_diff/math.go\"]");
		engine_set_ladybug_queries_enabled(1);
		check(lbug != nullptr && sqlite != nullptr,
		      "detect_changes null");
		// Both should report success (no error) and find modified
		// nodes.
		check(strstr(lbug, "\"error\":null") != nullptr,
		      "detect_changes ladybug error:null");
		check(strstr(sqlite, "\"error\":null") != nullptr,
		      "detect_changes sqlite error:null");
		// Compare name sets of callers and callees.
		bool match = sameNames(lbug, sqlite);
		if (!match) {
			fprintf(stderr, "  [DIFF] detect_changes\n");
			fprintf(stderr, "    ladybug: %s\n", lbug);
			fprintf(stderr, "    sqlite : %s\n", sqlite);
		}
		check(match,
		      "detect_changes diverges between LadybugDB and "
		      "SQLite");
		engine_free_string(lbug);
		engine_free_string(sqlite);
		++passed;
		fprintf(stderr, "  [PASS] detect_changes\n");
	}

	// ── traceCallChain: both paths agree on found + endpoints ──
	{
		total++;
		engine_set_ladybug_queries_enabled(1);
		char *lbug = engine_trace_call_chain(pid, "main", "add");
		engine_set_ladybug_queries_enabled(0);
		char *sqlite = engine_trace_call_chain(pid, "main", "add");
		engine_set_ladybug_queries_enabled(1);

		check(strstr(lbug, "\"found\":true") != nullptr,
		      "traceCallChain (ladybug) found");
		check(strstr(sqlite, "\"found\":true") != nullptr,
		      "traceCallChain (sqlite) found");
		check(strstr(lbug, "main") && strstr(lbug, "add"),
		      "traceCallChain (ladybug) endpoints");
		check(strstr(sqlite, "main") && strstr(sqlite, "add"),
		      "traceCallChain (sqlite) endpoints");

		engine_free_string(lbug);
		engine_free_string(sqlite);
		++passed;
		fprintf(stderr, "  [PASS] traceCallChain\n");
	}

	// ── findDefinition smoke (SQLite-only path, must still work) ──
	{
		total++;
		engine_set_ladybug_queries_enabled(1);
		char *def = engine_find_definition(pid, "add", nullptr);
		check(def != nullptr, "find_def add");
		check(strstr(def, "add") != nullptr, "find_def add contains name");
		engine_free_string(def);
		++passed;
		fprintf(stderr, "  [PASS] findDefinition\n");
	}

	// ── getGraphStats smoke (reports node/edge counts) ──
	{
		total++;
		engine_set_ladybug_queries_enabled(1);
		char *stats = engine_get_graph_stats(pid);
		check(stats != nullptr, "get_graph_stats");
		check(strstr(stats, "total_nodes") != nullptr,
		      "get_graph_stats has total_nodes");
		check(strstr(stats, "total_edges") != nullptr,
		      "get_graph_stats has total_edges");
		engine_free_string(stats);
		++passed;
		fprintf(stderr, "  [PASS] getGraphStats\n");
	}

	// ── Summary ──
	fprintf(stderr,
		"\n=== LadybugDB differential test: %d/%d passed ===\n",
		passed, total);

	engine_shutdown();
	std::filesystem::remove_all(proj_dir);
	unlink(db_path);
	unlink("/tmp/test_ladybug_diff.lbug");

	return passed == total ? 0 : 1;
}
