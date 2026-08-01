// test_homonym_filter.cpp — verify file_filter disambiguates homonyms.
//
// Step 2.3 (plan §Step 2): replaced the previous version that depended
// on the local /Users/scc/code/pycode/Transformer_Explorer project and
// returned 0 even on failure. This version is fully portable: it
// creates a tiny fixture in /tmp, indexes it, and verifies that
// file_filter narrows same-name (homonym) query results.
//
// Symptom (before disambiguation): engine_get_callees(pid, "handler")
// aggregates callees across ALL entities named "handler" in every file.
// This is noise — each file's "handler" is a distinct symbol.
//
// Fix under test: engine_get_callees(pid, "handler", file_filter)
// restricts the caller to the given file. With file_filter, callees
// should return only that file's handler callees, not the union.
//
// The fixture has two files, each defining a function named "handler"
// that calls a different helper. Without filter, callees("handler")
// should mention BOTH helpers. With filter, only the filtered file's
// helper should appear.
//
// Gate: returns nonzero if file_filter fails to reduce the result set
// or if the filtered result contains callees from the wrong file.

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
	char lbug_path[256];
	snprintf(lbug_path, sizeof(lbug_path), "%s.lbug", db_path);
	unlink(db_path);
	unlink(lbug_path);

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
	// Without filter, the query aggregates callees across ALL entities
	// named "handler". Both helperOne and helperTwo should appear.
	char *callees_no_filter = engine_get_callees(pid, "handler", nullptr);
	int total_no_filter = countTotal(callees_no_filter);
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
	printf("callees(handler) NO filter:    total=%d helperOne=%d helperTwo=%d\n",
	       total_no_filter, helperOne_no_filter, helperTwo_no_filter);
	printf("callees(handler) first.go:     total=%d helperOne=%d helperTwo=%d\n",
	       total_first, helperOne_first, helperTwo_first);
	printf("callees(handler) second.go:    total=%d helperOne=%d helperTwo=%d\n",
	       total_second, helperOne_second, helperTwo_second);

	// ── Gate ──────────────────────────────────────────────────────
	// The gate verifies that file_filter disambiguates homonyms:
	//   1. Without filter, both helpers appear (aggregation).
	//   2. With first.go filter, only helperOne appears.
	//   3. With second.go filter, only helperTwo appears.
	// Any failure returns nonzero (plan: "任何失败返回非零").
	bool pass = true;

	if (helperOne_no_filter < 1 || helperTwo_no_filter < 1) {
		printf("\nFAIL: without filter, both helpers should appear "
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
	// The filtered totals must be less than or equal to the unfiltered
	// total (filter narrows, never expands).
	if (total_first > total_no_filter || total_second > total_no_filter) {
		printf("\nFAIL: filter should narrow results "
		       "(no=%d first=%d second=%d)\n",
		       total_no_filter, total_first, total_second);
		pass = false;
	}

	if (pass) {
		printf("\nPASS: file_filter disambiguates homonyms "
		       "(no=%d, first=%d, second=%d)\n",
		       total_no_filter, total_first, total_second);
		engine_shutdown();
		return 0;
	}

	engine_shutdown();
	return 1;
}
