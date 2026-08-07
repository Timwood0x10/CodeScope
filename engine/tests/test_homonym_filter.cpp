// test_homonym_filter.cpp — verify homonym disambiguation.
//
// Step 2.3 (plan §Step 2): replaced the previous version that depended
// on the local /Users/scc/code/pycode/Transformer_Explorer project and
// returned 0 even on failure. This version is fully portable: it
// creates a tiny fixture in /tmp, indexes it, and verifies homonym
// handling end to end.
//
// Step 7 (plan §7.3): the bare-name API no longer silently aggregates
// across all entities that share a name. When multiple entities match
// the bare name, getCallers/getCallees return ambiguous=true with a
// candidate list; a file_filter that narrows to a single entity
// proceeds normally. This test verifies BOTH behaviors:
//
//   - Without a filter, "handler" is ambiguous (2 entities) — the query
//     must return ambiguous=true + candidates, not merged callees.
//   - With file_filter=first.go, only first.go's handler is queried and
//     only helperOne appears.
//   - With file_filter=second.go, only helperTwo appears.
//
// The fixture has two files, each defining a function named "handler"
// that calls a different helper.
//
// Gate: returns nonzero on any failure.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

/// Count occurrences of a substring in a JSON string.
static int countOccurrences(const char *json, const char *needle)
{
	if (!json || !needle)
		return 0;
	int count = 0;
	const char *p = json;
	size_t nlen = strlen(needle);
	while ((p = strstr(p, needle)) != nullptr) {
		++count;
		p += nlen;
	}
	return count;
}

/// Extract the "total":N field from a JSON response.
static int countTotal(const char *json)
{
	if (!json)
		return -1;
	const char *p = strstr(json, "\"total\":");
	if (!p)
		return -1;
	return atoi(p + 8);
}

int main()
{
	// ── Build a portable fixture in /tmp ──────────────────────────
	// Two Go files, each with a function named "handler" that calls
	// a distinct helper. This creates a homonym: same name, different
	// entities, different callees.
	const char *proj_dir = "/tmp/test_homonym_filter";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	{
		FILE *f = fopen((std::string(proj_dir) + "/first.go").c_str(),
				"w");
		if (!f) {
			fprintf(stderr, "FAIL: cannot create first.go\n");
			return 1;
		}
		fputs("package main\n\n"
		      "// handler in first.go calls helperOne.\n"
		      "func handler() int {\n"
		      "    return helperOne()\n"
		      "}\n"
		      "func helperOne() int { return 1 }\n",
		      f);
		fclose(f);
	}
	{
		FILE *f = fopen((std::string(proj_dir) + "/second.go").c_str(),
				"w");
		if (!f) {
			fprintf(stderr, "FAIL: cannot create second.go\n");
			return 1;
		}
		fputs("package main\n\n"
		      "// handler in second.go calls helperTwo.\n"
		      "func handler() int {\n"
		      "    return helperTwo()\n"
		      "}\n"
		      "func helperTwo() int { return 2 }\n",
		      f);
		fclose(f);
	}

	const char *db_path = "/tmp/test_homonym_filter.db";
	unlink(db_path);

	if (engine_init(db_path) != 0) {
		fprintf(stderr, "FAIL: engine_init\n");
		return 1;
	}

	uint64_t pid = engine_create_project(proj_dir, "homonym-test");
	if (pid == 0) {
		fprintf(stderr, "FAIL: engine_create_project\n");
		engine_shutdown();
		return 1;
	}

	char *idx = engine_index_project(pid, proj_dir, nullptr);
	if (!idx || !strstr(idx, "\"ok\":true")) {
		fprintf(stderr, "FAIL: engine_index_project: %s\n",
			idx ? idx : "(null)");
		engine_free_string(idx);
		engine_shutdown();
		return 1;
	}
	engine_free_string(idx);

	// Allow the synchronous graph build to settle.
	usleep(200000);

	std::string first_file = std::string(proj_dir) + "/first.go";
	std::string second_file = std::string(proj_dir) + "/second.go";

	// ── Test 1: callees("handler") WITHOUT file_filter ───────────
	// Step 7 semantics: two entities named "handler" exist, so the bare
	// name is ambiguous. The API must return ambiguous=true with a
	// candidate list instead of silently merging both callees.
	char *callees_no_filter = engine_get_callees(pid, "handler", nullptr);
	bool no_filter_ambiguous =
		callees_no_filter && strstr(callees_no_filter, "\"ambiguous\":true");
	int candidates_count = countOccurrences(
		callees_no_filter ? callees_no_filter : "", "\"graph_node_id\"");
	int helperOne_no_filter =
		countOccurrences(callees_no_filter, "helperOne");
	int helperTwo_no_filter =
		countOccurrences(callees_no_filter, "helperTwo");
	printf("--- callees(handler) NO filter ---\n%s\n\n",
	       callees_no_filter ? callees_no_filter : "(null)");
	engine_free_string(callees_no_filter);

	// ── Test 2: callees("handler") WITH file_filter=first.go ─────
	// With filter, only first.go's handler is queried. Only helperOne
	// should appear; helperTwo must NOT appear.
	char *callees_first =
		engine_get_callees(pid, "handler", first_file.c_str());
	int total_first = countTotal(callees_first);
	int helperOne_first = countOccurrences(callees_first, "helperOne");
	int helperTwo_first = countOccurrences(callees_first, "helperTwo");
	printf("--- callees(handler) WITH filter (%s) ---\n%s\n\n",
	       first_file.c_str(), callees_first ? callees_first : "(null)");
	engine_free_string(callees_first);

	// ── Test 3: callees("handler") WITH file_filter=second.go ────
	char *callees_second =
		engine_get_callees(pid, "handler", second_file.c_str());
	int total_second = countTotal(callees_second);
	int helperOne_second = countOccurrences(callees_second, "helperOne");
	int helperTwo_second = countOccurrences(callees_second, "helperTwo");
	printf("--- callees(handler) WITH filter (%s) ---\n%s\n\n",
	       second_file.c_str(), callees_second ? callees_second : "(null)");
	engine_free_string(callees_second);

	printf("=== SUMMARY ===\n");
	printf("callees(handler) NO filter:  ambiguous=%s candidates=%d "
	       "helperOne=%d helperTwo=%d\n",
	       no_filter_ambiguous ? "true" : "false", candidates_count,
	       helperOne_no_filter, helperTwo_no_filter);
	printf("callees(handler) first.go:     total=%d helperOne=%d helperTwo=%d\n",
	       total_first, helperOne_first, helperTwo_first);
	printf("callees(handler) second.go:    total=%d helperOne=%d helperTwo=%d\n",
	       total_second, helperOne_second, helperTwo_second);

	// ── Gate ──────────────────────────────────────────────────────
	// 1. Without filter, the bare name is ambiguous: the response must
	//    carry ambiguous=true and list 2 candidates, and must NOT merge
	//    callees from both files (helperOne/helperTwo absent from the
	//    callee list).
	// 2. With first.go filter, only helperOne appears.
	// 3. With second.go filter, only helperTwo appears.
	// Any failure returns nonzero (plan: "任何失败返回非零").
	bool pass = true;

	if (!no_filter_ambiguous) {
		printf("\nFAIL: without filter, homonym should be ambiguous "
		       "(ambiguous=true expected)\n");
		pass = false;
	}
	if (candidates_count != 2) {
		printf("\nFAIL: without filter, expected 2 candidates "
		       "(got %d)\n",
		       candidates_count);
		pass = false;
	}
	if (helperOne_no_filter > 0 || helperTwo_no_filter > 0) {
		printf("\nFAIL: without filter, callees must NOT be merged "
		       "(helperOne=%d helperTwo=%d)\n",
		       helperOne_no_filter, helperTwo_no_filter);
		pass = false;
	}
	if (helperOne_first < 1) {
		printf("\nFAIL: first.go filter should include helperOne "
		       "(got helperOne=%d)\n",
		       helperOne_first);
		pass = false;
	}
	if (helperTwo_first > 0) {
		printf("\nFAIL: first.go filter should NOT include helperTwo "
		       "(got helperTwo=%d)\n",
		       helperTwo_first);
		pass = false;
	}
	if (helperTwo_second < 1) {
		printf("\nFAIL: second.go filter should include helperTwo "
		       "(got helperTwo=%d)\n",
		       helperTwo_second);
		pass = false;
	}
	if (helperOne_second > 0) {
		printf("\nFAIL: second.go filter should NOT include helperOne "
		       "(got helperOne=%d)\n",
		       helperOne_second);
		pass = false;
	}

	if (pass) {
		printf("\nPASS: homonym disambiguation works "
		       "(ambiguous=%s candidates=%d first=%d second=%d)\n",
		       no_filter_ambiguous ? "true" : "false",
		       candidates_count, total_first, total_second);
		engine_shutdown();
		return 0;
	}

	engine_shutdown();
	return 1;
}
