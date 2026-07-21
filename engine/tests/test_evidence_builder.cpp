// test_evidence_builder: verify EvidenceBuilder turns semantic_fact
// rows into Evidence findings via the rule files under
// engine/src/evidence/rules/. The test inserts facts that match each
// rule's (category, primitive, kind) triples, then asserts the
// builder produces the expected evidence counts.
//
// Covered rules:
//   1. sync/mutex_without_defer_unlock (missing_match)
//      - function 1: lock WITHOUT defer_unlock  → 1 evidence item
//      - function 2: lock WITH defer_unlock     → excluded
//   2. memory/cstring_leak (missing_match_per_function)
//      - function 3: alloc WITHOUT free → 1 evidence
//      - function 4: alloc WITH free    → excluded
//   3. error/bare_except_collect (collect) → 1 evidence item
//   4. pattern/todo_collect (collect)      → 1 evidence item
//   5. pattern/unwrap_risk (collect)       → 1 evidence item (bonus)
//
// Test flow:
//   1. Open a temp GraphStore at /tmp/test_evidence_builder.db
//   2. createSchema + createProject
//   3. Insert graph_nodes (functions) for each test case
//   4. Insert semantic_fact rows matching each rule's needs
//   5. Create EvidenceBuilder, loadRules (path tried from a list)
//   6. Run buildAll, verify expected evidence counts
//   7. Run buildByCategory("sync"), verify only sync evidence
//   8. Run buildByRule("cstring_leak"), verify 1 evidence
//   9. Cleanup: close store, unlink temp DB

#include "../src/evidence/evidence_builder.h"
#include "../src/store/store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace evidence;
using namespace store;

static const char *kDbPath = "/tmp/test_evidence_builder.db";

/// Insert a graph_node function row with the given id, name, file_path,
/// language. start_row/end_row are wide enough (1..1000) so any
/// semantic_fact referencing this function_id via FK is valid.
static void insertFunction(GraphStore &store, uint64_t project_id, int64_t id,
			   const char *name, const char *file_path,
			   const char *language)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO graph_nodes (id, project_id, ir_node_id, "
		"node_type, name, qualified_name, file_path, language, "
		"start_row, start_col, end_row, end_col) "
		"VALUES (?,?,0,0,?,'',?,?,1,0,1000,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, language, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a semantic_fact row directly. Bypasses the extractor so the
/// test can control the exact (category, primitive, kind) triple and
/// exercise each rule's combine mode precisely.
static void insertFact(GraphStore &store, uint64_t project_id,
		       uint64_t function_id, const char *category,
		       const char *primitive, const char *kind,
		       const char *symbol, double confidence,
		       const char *detail_json)
{
	sqlite3 *db = store.handle();
	const char *sql =
		"INSERT INTO semantic_fact "
		"(project_id, function_id, category, primitive, kind, "
		" symbol, confidence, detail_json) "
		"VALUES (?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(function_id));
	sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, primitive, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, symbol, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(stmt, 7, confidence);
	if (detail_json && *detail_json)
		sqlite3_bind_text(stmt, 8, detail_json, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 8);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Build a detail_json string in the format written by
/// SemanticFactExtractor::buildDetailJson:
///   {"line":N,"snippet":"name (file_path)","related_symbol":"..."}
static std::string detailJson(int line, const std::string &snippet,
			      const std::string &related_symbol)
{
	std::string out = "{\"line\":" + std::to_string(line) +
			  ",\"snippet\":\"" + snippet +
			  "\",\"related_symbol\":\"" + related_symbol + "\"}";
	return out;
}

/// Find the rules directory by trying a list of candidate paths.
/// Returns the first path that exists and contains *.json files; on
/// failure returns an empty string. The test binary may run from
/// engine/build/ (../src/evidence/rules works) or from a deeper
/// build subdirectory (../../engine/src/evidence/rules). A hard-coded
/// absolute path is the last resort for the dev environment.
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
		// Verify the directory actually contains *.json files.
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

int main()
{
	unlink(kDbPath);

	GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	uint64_t pid = store.createProject("/tmp", "test_evidence_builder");
	assert(pid > 0);

	// ── Test 1: sync/mutex_without_defer_unlock (missing_match) ──
	// Function 1: lock WITHOUT defer_unlock → should appear in
	// evidence (the leak case).
	insertFunction(store, pid, 100, "AcquireLeak", "/src/sync_leak.go",
		       "go");
	insertFact(store, pid, 100, "sync", "mutex", "lock", "m.Lock", 1.0,
		   detailJson(5, "m.Lock (/src/sync_leak.go)", "").c_str());

	// Function 2: lock WITH defer_unlock → should be EXCLUDED from
	// evidence (no leak).
	insertFunction(store, pid, 101, "AcquireSafe", "/src/sync_safe.go",
		       "go");
	insertFact(store, pid, 101, "sync", "mutex", "lock", "m.Lock", 1.0,
		   detailJson(3, "m.Lock (/src/sync_safe.go)", "").c_str());
	insertFact(
		store, pid, 101, "sync", "mutex", "defer_unlock",
		"defer m.Unlock", 1.0,
		detailJson(4, "defer m.Unlock (/src/sync_safe.go)", "").c_str());

	// ── Test 2: memory/cstring_leak (missing_match_per_function) ─
	// Function 3: alloc WITHOUT free → 1 evidence (leak).
	insertFunction(store, pid, 200, "ToStringLeak", "/src/cgo_leak.go",
		       "go");
	insertFact(store, pid, 200, "memory", "cstring", "alloc", "C.CString",
		   1.0,
		   detailJson(9, "C.CString (/src/cgo_leak.go)", "").c_str());

	// Function 4: alloc WITH free → EXCLUDED (no leak).
	insertFunction(store, pid, 201, "ToStringSafe", "/src/cgo_safe.go",
		       "go");
	insertFact(store, pid, 201, "memory", "cstring", "alloc", "C.CString",
		   1.0,
		   detailJson(9, "C.CString (/src/cgo_safe.go)", "").c_str());
	insertFact(store, pid, 201, "memory", "cstring", "free", "C.free", 1.0,
		   detailJson(10, "C.free (/src/cgo_safe.go)", "").c_str());

	// ── Test 3: error/bare_except_collect (collect) ──────────────
	insertFunction(store, pid, 300, "RiskyExcept", "/src/risky.py",
		       "python");
	insertFact(store, pid, 300, "error", "bare_except", "suppression",
		   "except", 0.9,
		   detailJson(7, "except (/src/risky.py)", "python").c_str());

	// ── Test 4: pattern/todo_collect (collect) ───────────────────
	insertFunction(store, pid, 400, "StubFunction", "/src/stub.go", "go");
	insertFact(
		store, pid, 400, "pattern", "todo", "marker", "TODO: implement",
		1.0,
		detailJson(11, "TODO: implement (/src/stub.go)", "").c_str());

	// ── Test 5: pattern/unwrap_risk (collect, bonus) ────────────
	insertFunction(store, pid, 500, "MaybeCrash", "/src/unwrap.rs", "rust");
	insertFact(
		store, pid, 500, "pattern", "unwrap", "risk", "result.unwrap",
		0.8,
		detailJson(15, "result.unwrap (/src/unwrap.rs)", "").c_str());

	// ── Load rules ───────────────────────────────────────────────
	std::string rules_dir = findRulesDir();
	if (rules_dir.empty()) {
		fprintf(stderr, "FAIL: cannot find evidence rules directory\n");
		store.close();
		unlink(kDbPath);
		return 1;
	}
	printf("Using rules dir: %s\n", rules_dir.c_str());

	EvidenceBuilder builder(&store);
	builder.loadRules(rules_dir);
	if (builder.ruleSets().empty()) {
		fprintf(stderr, "FAIL: no rule sets loaded from %s\n",
			rules_dir.c_str());
		store.close();
		unlink(kDbPath);
		return 1;
	}
	printf("Loaded %zu rule sets\n", builder.ruleSets().size());

	// ── Run buildAll ─────────────────────────────────────────────
	auto all = builder.buildAll(pid);
	printf("buildAll returned %zu evidence(s)\n", all.size());
	assert(!all.empty());

	// Helper: find an evidence by substring in its title. Each
	// rule's title template is unique enough that a substring match
	// unambiguously identifies the rule's output evidence.
	auto findByTitleContains =
		[&](const std::string &needle) -> const Evidence * {
		for (const auto &ev : all) {
			if (ev.title.find(needle) != std::string::npos)
				return &ev;
		}
		return nullptr;
	};

	// ── Verify: mutex_without_defer_unlock has 1 item ────────────
	// The title template is "{count} function(s) lock mutex without
	// defer Unlock" — substituted to "1 function(s) lock mutex
	// without defer Unlock".
	{
		const Evidence *ev =
			findByTitleContains("lock mutex without defer Unlock");
		assert(ev != nullptr);
		assert(ev->items.size() == 1);
		assert(ev->items[0].symbol == "m.Lock");
		assert(ev->items[0].line == 5);
		assert(ev->items[0].file == "/src/sync_leak.go");
		printf("Test 1 (mutex_without_defer_unlock, 1 item): "
		       "PASS\n");
	}

	// ── Verify: cstring_leak has 1 evidence (per_function mode) ──
	// Function 3 leaks, function 4 does not — so exactly 1 evidence
	// should be emitted (one per surviving function).
	{
		auto cstring_evs = builder.buildByRule(pid, "cstring_leak");
		assert(cstring_evs.size() == 1);
		assert(cstring_evs[0].items.size() == 1);
		assert(cstring_evs[0].items[0].symbol == "C.CString");
		assert(cstring_evs[0].items[0].file == "/src/cgo_leak.go");
		printf("Test 2 (cstring_leak, 1 evidence 1 item): "
		       "PASS\n");
	}

	// ── Verify: bare_except_collect has 1 item ───────────────────
	{
		const Evidence *ev =
			findByTitleContains("bare except clause(s)");
		assert(ev != nullptr);
		assert(ev->items.size() == 1);
		assert(ev->items[0].symbol == "except");
		printf("Test 3 (bare_except_collect, 1 item): PASS\n");
	}

	// ── Verify: todo_collect has 1 item ──────────────────────────
	{
		const Evidence *ev = findByTitleContains("TODO marker(s)");
		assert(ev != nullptr);
		assert(ev->items.size() == 1);
		assert(ev->items[0].symbol == "TODO: implement");
		printf("Test 4 (todo_collect, 1 item): PASS\n");
	}

	// ── Verify: unwrap_risk has 1 item (bonus) ───────────────────
	{
		const Evidence *ev = findByTitleContains(".unwrap() call(s)");
		assert(ev != nullptr);
		assert(ev->items.size() == 1);
		assert(ev->items[0].symbol == "result.unwrap");
		printf("Test 5 (unwrap_risk, 1 item): PASS\n");
	}

	// ── Test 6: buildByCategory("sync") returns only sync evidence ─
	{
		auto sync_evs = builder.buildByCategory(pid, "sync");
		assert(!sync_evs.empty());
		for (const auto &ev : sync_evs) {
			assert(ev.category == "sync");
		}
		// The sync category has only the mutex_without_defer_unlock
		// rule, which produced 1 evidence.
		assert(sync_evs.size() == 1);
		printf("Test 6 (buildByCategory sync, %zu evidence): "
		       "PASS\n",
		       sync_evs.size());
	}

	// ── Test 7: buildByRule("cstring_leak") returns 1 evidence ───
	// (Already verified above in Test 2; repeat with explicit
	// count assertion.)
	{
		auto evs = builder.buildByRule(pid, "cstring_leak");
		assert(evs.size() == 1);
		printf("Test 7 (buildByRule cstring_leak, 1 evidence): "
		       "PASS\n");
	}

	// ── Test 8: buildByRule with unknown name returns 0 ──────────
	{
		auto evs = builder.buildByRule(pid, "nonexistent_rule");
		assert(evs.empty());
		printf("Test 8 (buildByRule unknown, 0 evidence): "
		       "PASS\n");
	}

	// ── Test 9: buildByCategory with no matching category ────────
	{
		auto evs = builder.buildByCategory(pid, "nonexistent");
		assert(evs.empty());
		printf("Test 9 (buildByCategory unknown, 0 evidence): "
		       "PASS\n");
	}

	// ── Sanity: total evidence count matches expected rules ──────
	// Existing categories (5):
	//   mutex_without_defer_unlock (1) + cstring_leak (1,
	//   per_function) + bare_except_collect (1) + todo_collect (1)
	//   + unwrap_risk (1)
	// Phase 5 domain rules firing on the same facts (5):
	//   drift/dead_pattern_todo (1, TODO inserted)
	//   security/bare_except_security (1, bare_except inserted)
	//   concurrency/mutex_without_unlock (1, lock without
	//     defer_unlock in function 100)
	//   test_quality/unwrap_in_non_test (1, unwrap inserted)
	//   test_quality/todo_accumulation (1, Count mode always emits
	//     1 evidence)
	// Total = 5 + 5 = 10.
	assert(all.size() == 10);
	printf("Test 10 (total evidence count == 10): PASS\n");

	store.close();
	unlink(kDbPath);
	printf("\nAll evidence_builder tests passed.\n");
	return 0;
}
