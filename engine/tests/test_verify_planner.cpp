// test_verify_planner: verify the Phase 3 Verification Planner
// pipeline (IntentParser → Planner → VerdictBuilder → FFI).
//
// Test flow:
//   1. Test IntentParser in isolation (no store needed):
//      - "this project has a bare except clause" → pattern_question, 1 req
//      - "does this project safely handle CString?" → safety_question, 3 reqs
//      - "xyz random text" → unknown
//   2. Initialize the engine on a temp DB.
//   3. Insert semantic_fact rows for:
//      - a mutex lock WITHOUT defer_unlock (sync leak)
//      - a cstring alloc WITHOUT free (memory leak)
//      - a bare_except (error suppression)
//   4. Test the full FFI pipeline:
//      - engine_verify_statement(pid, "this project has a bare except
//        clause") → verdict != Unknown, JSON contains "verdict"
//      - engine_verify_statement(pid, "safely handle CString") →
//        verdict is one of Supported/Contradicted/PartiallyVerified
//   5. Cleanup.

#include "../include/engine.h"
#include "../src/engine_internal.h"
#include "../src/evidence/evidence_builder.h"
#include "../src/store/store.h"
#include "../src/verify/intent_parser.h"
#include "../src/verify/planner.h"
#include "../src/verify/verdict_builder.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

using namespace verify::planner;

static const char *kDbPath = "/tmp/test_verify_planner.db";

/// Find the rules directory by trying a list of candidate paths.
/// Mirrors the helper in test_evidence_builder.cpp.
static std::string findRulesDir()
{
	const char *candidates[] = {
	   "../src/evidence/rules",
	   "../../engine/src/evidence/rules",
	   "../../../engine/src/evidence/rules",
	  };
	for (const char *cand : candidates) {
		std::error_code ec;
		if (!std::filesystem::is_directory(cand, ec))
			continue;
		bool has_json = false;
		for (const auto &entry :
		     std::filesystem::directory_iterator(cand, ec)) {
			if (entry.is_regular_file() &&
			    entry.path().extension() == ".json") {
				has_json = true;
				break;
			}
		}
		if (has_json)
			return cand;
	}
	return "";
}

/// Insert a graph_node function row. Mirrors the helper in
/// test_evidence_builder.cpp.
static void insertFunction(store::GraphStore &store, uint64_t project_id,
			   int64_t id, const char *name,
			   const char *file_path, const char *language)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, file_path, language, "
		"start_row, start_col, end_row, end_col) "
		"VALUES (?,?,0,0,?,'',?,?,1,0,1000,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, language, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a semantic_fact row directly. Mirrors the helper in
/// test_evidence_builder.cpp.
static void
insertFact(store::GraphStore &store, uint64_t project_id,
	   uint64_t function_id, const char *category,
	   const char *primitive, const char *kind, const char *symbol,
	   double confidence, const char *detail_json)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO semantic_fact "
		"(project_id, function_id, category, primitive, kind, "
		" symbol, confidence, detail_json) "
		"VALUES (?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(function_id));
	sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, primitive, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, symbol, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(stmt, 7, confidence);
	if (detail_json && *detail_json)
		sqlite3_bind_text(stmt, 8, detail_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 8);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Build a detail_json string in the format written by
/// SemanticFactExtractor::buildDetailJson.
static std::string detailJson(int line, const std::string &snippet,
			      const std::string &related_symbol)
{
	return "{\"line\":" + std::to_string(line) +
	       ",\"snippet\":\"" + snippet +
	       "\",\"related_symbol\":\"" + related_symbol + "\"}";
}

/// True if `haystack` contains `needle`.
static bool contains(const char *haystack, const char *needle)
{
	return strstr(haystack, needle) != nullptr;
}

int main()
{
	// ── Phase A: Test IntentParser in isolation ──────────────────
	// These tests don't need the engine or any DB — IntentParser
	// is a pure text→Intent transformer.
	printf("Phase A: IntentParser tests\n");
	{
		IntentParser parser;

		// Test 1: "this project has a bare except clause"
		// → pattern_question, 1 requirement (PatternMatch)
		{
			Intent intent = parser.parse(
				"this project has a bare except clause");
			assert(intent.type == "pattern_question");
			assert(intent.requirements.size() == 1);
			assert(intent.requirements[0].id == "PatternMatch");
			assert(intent.requirements[0].rule_names.size() ==
			       1);
			assert(intent.requirements[0].rule_names[0] ==
			       "bare_except_collect");
			printf("  Test 1 (bare except → pattern_question, "
			       "1 req): PASS\n");
		}

		// Test 2: "does this project safely handle CString?"
		// → safety_question, 3 requirements (MemoryOwnership,
		// FFIBoundary, Lifetime)
		{
			Intent intent = parser.parse(
				"does this project safely handle CString?");
			assert(intent.type == "safety_question");
			assert(intent.requirements.size() == 3);
			assert(intent.requirements[0].id ==
			       "MemoryOwnership");
			assert(intent.requirements[0].weight == 0.5);
			assert(intent.requirements[1].id == "FFIBoundary");
			assert(intent.requirements[1].weight == 0.3);
			assert(intent.requirements[2].id == "Lifetime");
			assert(intent.requirements[2].weight == 0.2);
			printf("  Test 2 (safely handle CString → "
			       "safety_question, 3 reqs): PASS\n");
		}

		// Test 3: "xyz random text" → unknown, 0 requirements
		{
			Intent intent = parser.parse("xyz random text");
			assert(intent.type == "unknown");
			assert(intent.requirements.empty());
			printf("  Test 3 (xyz random text → unknown): "
			       "PASS\n");
		}
	}

	// ── Phase B: Initialize engine + insert facts ───────────────
	printf("Phase B: engine init + fact insertion\n");
	unlink(kDbPath);
	unlink("/tmp/test_verify_planner.db-wal");
	unlink("/tmp/test_verify_planner.db-shm");

	int rc = engine_init(kDbPath);
	assert(rc == 0 && "engine_init should succeed");
	assert(g_store && "g_store should be non-null after engine_init");

	uint64_t pid = engine_create_project("/tmp/test-verify-planner",
					     "test-verify-planner");
	assert(pid > 0 && "engine_create_project should return positive id");

	// Verify the rules directory is findable. The FFI uses
	// CODESCOPE_RULES_DIR or the default "engine/src/evidence/rules"
	// relative to CWD; set the env var to the absolute path so the
	// test works regardless of CWD.
	std::string rules_dir = findRulesDir();
	if (rules_dir.empty()) {
		fprintf(stderr,
			"FAIL: cannot find evidence rules directory\n");
		engine_shutdown();
		unlink(kDbPath);
		return 1;
	}
	setenv("CODESCOPE_RULES_DIR", rules_dir.c_str(), 1);
	printf("  Using rules dir: %s\n", rules_dir.c_str());

	// Insert a function + a mutex lock fact WITHOUT defer_unlock.
	// This matches the mutex_without_defer_unlock rule.
	insertFunction(*g_store, pid, 100, "AcquireLeak",
		       "/src/sync_leak.go", "go");
	insertFact(*g_store, pid, 100, "sync", "mutex", "lock", "m.Lock",
		   1.0,
		   detailJson(5, "m.Lock (/src/sync_leak.go)", "").c_str());

	// Insert a function + a cstring alloc fact WITHOUT free.
	// This matches the cstring_leak rule.
	insertFunction(*g_store, pid, 200, "ToStringLeak",
		       "/src/cgo_leak.go", "go");
	insertFact(*g_store, pid, 200, "memory", "cstring", "alloc",
		   "C.CString", 1.0,
		   detailJson(9, "C.CString (/src/cgo_leak.go)", "")
		       .c_str());

	// Insert a function + a bare_except fact.
	// This matches the bare_except_collect rule.
	insertFunction(*g_store, pid, 300, "RiskyExcept",
		       "/src/risky.py", "python");
	insertFact(*g_store, pid, 300, "error", "bare_except",
		   "suppression", "except", 0.9,
		   detailJson(7, "except (/src/risky.py)", "python")
		       .c_str());
	printf("  Inserted 3 semantic_fact rows (mutex lock, cstring "
	       "alloc, bare_except)\n");

	// ── Phase C: Test full FFI pipeline ─────────────────────────
	printf("Phase C: engine_verify_statement tests\n");

	// Test 4: "this project has a bare except clause"
	// → verdict != Unknown, JSON contains "verdict"
	{
		char *result = engine_verify_statement(
			pid, "this project has a bare except clause");
		assert(result && "FFI must return non-null");
		assert(contains(result, "\"verdict\"") &&
		       "result must contain a verdict field");
		// The verdict must NOT be Unknown because the
		// bare_except_collect rule matches the inserted fact.
		assert(!contains(result, "\"verdict\":\"Unknown\"") &&
		       "verdict must not be Unknown when evidence exists");
		printf("  Test 4 (bare except → non-Unknown verdict): "
		       "PASS\n");
		printf("    Result: %s\n", result);
		engine_free_string(result);
	}

	// Test 5: "safely handle CString"
	// → verdict is one of Supported/Contradicted/PartiallyVerified
	// (NOT Unknown, because the MemoryOwnership requirement's
	// cstring_leak rule matches the inserted cstring alloc fact.)
	{
		char *result = engine_verify_statement(
			pid, "safely handle CString");
		assert(result && "FFI must return non-null");
		assert(contains(result, "\"verdict\"") &&
		       "result must contain a verdict field");
		// Build the list of acceptable verdicts.
		bool is_supported = contains(result, "\"verdict\":\"Supported\"");
		bool is_contradicted = contains(result, "\"verdict\":\"Contradicted\"");
		bool is_partial = contains(result, "\"verdict\":\"PartiallyVerified\"");
		bool is_unknown = contains(result, "\"verdict\":\"Unknown\"");
		assert((is_supported || is_contradicted || is_partial) &&
		       "verdict must be Supported/Contradicted/PartiallyVerified");
		assert(!is_unknown &&
		       "verdict must not be Unknown when cstring evidence exists");
		printf("  Test 5 (safely handle CString → "
		       "Supported/Contradicted/PartiallyVerified): PASS\n");
		printf("    Result: %s\n", result);
		engine_free_string(result);
	}

	// Test 6: "xyz random text" → verdict is Unknown (no
	// requirements matched because the Intent type is "unknown").
	{
		char *result = engine_verify_statement(pid,
						       "xyz random text");
		assert(result && "FFI must return non-null");
		assert(contains(result, "\"verdict\":\"Unknown\"") &&
		       "unknown claim must return Unknown verdict");
		printf("  Test 6 (xyz random text → Unknown): PASS\n");
		printf("    Result: %s\n", result);
		engine_free_string(result);
	}

	// ── Phase D: Cleanup ────────────────────────────────────────
	printf("Phase D: cleanup\n");
	engine_shutdown();
	unlink(kDbPath);
	unlink("/tmp/test_verify_planner.db-wal");
	unlink("/tmp/test_verify_planner.db-shm");
	printf("\nAll verify_planner tests passed.\n");
	return 0;
}
