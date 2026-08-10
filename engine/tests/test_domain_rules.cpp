// test_domain_rules: verify the Phase 5 v0.3 domain rule files
// (drift.json, security.json, concurrency.json, test_quality.json)
// load via the existing RuleLoader and produce Evidence findings for
// each new category.
//
// Covered categories + rules:
//   drift       — dead_pattern_todo (collect) fires on a TODO marker
//   security    — unsafe_code_risk (collect) fires on an unsafe block,
//                  unchecked_ffi (collect) fires on an extern_call,
//                  bare_except_security (collect) fires on a bare except
//   concurrency — mutex_without_unlock (missing_match) fires on a
//                  mutex lock WITHOUT defer_unlock
//   test_quality — unwrap_in_non_test (collect) fires on an unwrap,
//                  todo_accumulation (count) emits 1 evidence with
//                  count = number of TODOs
//
// Test flow:
//   1. Open a temp GraphStore at /tmp/test_domain_rules.db
//   2. createSchema + createProject
//   3. Insert graph_nodes (functions) for each test case
//   4. Insert semantic_fact rows: TODO, unsafe block, mutex lock
//      (no defer_unlock), unwrap, bare except, extern_call
//   5. Create EvidenceBuilder, loadRules from
//      engine/src/evidence/rules (path tried from a list of
//      candidates — see findRulesDir)
//   6. Assert total rule sets >= 10 (6 existing + 4 new)
//   7. Run buildAll; verify at least 1 evidence per new category
//      (drift / security / concurrency / test_quality)
//   8. Run buildByCategory("concurrency"); verify it returns only
//      concurrency evidence
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

static const char *kDbPath = "/tmp/test_domain_rules.db";

/// Insert a graph_node function row with the given id, name, file_path,
/// language. start_row/end_row span a wide range so any semantic_fact
/// referencing this function_id via FK is valid.
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
/// test can control the exact (category, primitive, kind) triple.
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
	return "{\"line\":" + std::to_string(line) + ",\"snippet\":\"" +
	       snippet + "\",\"related_symbol\":\"" + related_symbol + "\"}";
}

/// Find the rules directory by trying a list of candidate paths.
/// Returns the first path that exists and contains *.json files; on
/// failure returns an empty string. Mirrors the helper in
/// test_evidence_builder.cpp so the test can run from engine/build/
/// or from a deeper build subdirectory.
static std::string findRulesDir()
{
	const char *candidates[] = {
	   "engine/src/evidence/rules",
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

/// Count evidences whose category matches `cat` in the given list.
static size_t countByCategory(const std::vector<Evidence> &evs,
			      const std::string &cat)
{
	size_t n = 0;
	for (const auto &ev : evs)
		if (ev.category == cat)
			++n;
	return n;
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
	uint64_t pid = store.createProject("/tmp", "test_domain_rules");
	assert(pid > 0);

	// ── Insert one function per test fact (different function_ids
	// so missing_match's per-function exclusion logic is exercised
	// precisely). Each function gets exactly one semantic_fact.
	// ───────────────────────────────────────────────────────────

	// Fact 1: pattern/todo/marker — feeds drift/dead_pattern_todo
	// and test_quality/todo_accumulation.
	insertFunction(store, pid, 1001, "StubFn", "/src/stub.go", "go");
	insertFact(
		store, pid, 1001, "pattern", "todo", "marker",
		"TODO: implement", 1.0,
		detailJson(11, "TODO: implement (/src/stub.go)", "").c_str());

	// Fact 2: pattern/unsafe/risk — feeds security/unsafe_code_risk.
	insertFunction(store, pid, 1002, "UnsafeBlock", "/src/unsafe.rs",
		       "rust");
	insertFact(store, pid, 1002, "pattern", "unsafe", "risk", "unsafe {}",
		   0.9,
		   detailJson(7, "unsafe {} (/src/unsafe.rs)", "rust").c_str());

	// Fact 3: sync/mutex/lock WITHOUT defer_unlock — feeds
	// concurrency/mutex_without_unlock (missing_match fires).
	insertFunction(store, pid, 1003, "LockLeak", "/src/lock_leak.go", "go");
	insertFact(store, pid, 1003, "sync", "mutex", "lock", "m.Lock", 1.0,
		   detailJson(5, "m.Lock (/src/lock_leak.go)", "").c_str());

	// Fact 4: pattern/unwrap/risk — feeds
	// test_quality/unwrap_in_non_test.
	insertFunction(store, pid, 1004, "UnwrapCall", "/src/unwrap.rs",
		       "rust");
	insertFact(
		store, pid, 1004, "pattern", "unwrap", "risk", "result.unwrap",
		0.8,
		detailJson(15, "result.unwrap (/src/unwrap.rs)", "").c_str());

	// Fact 5: error/bare_except/suppression — feeds
	// security/bare_except_security.
	insertFunction(store, pid, 1005, "BareExcept", "/src/bare.py",
		       "python");
	insertFact(store, pid, 1005, "error", "bare_except", "suppression",
		   "except", 0.9,
		   detailJson(7, "except (/src/bare.py)", "python").c_str());

	// Fact 6: ffi/extern_call/call — feeds
	// security/unchecked_ffi.
	insertFunction(store, pid, 1006, "ExternCall", "/src/ffi.cpp", "cpp");
	insertFact(store, pid, 1006, "ffi", "extern_call", "call",
		   "extern \"C\"", 1.0,
		   detailJson(20, "extern \"C\" (/src/ffi.cpp)", "").c_str());

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

	// ── Test 1: at least 10 rule sets (6 existing + 4 new) ───────
	assert(builder.ruleSets().size() >= 10);
	printf("Test 1 (rule sets >= 10, got %zu): PASS\n",
	       builder.ruleSets().size());

	// ── Test 2: all 4 new categories are present ─────────────────
	auto hasCategory = [&](const std::string &cat) -> bool {
		for (const auto &rs : builder.ruleSets())
			if (rs.category == cat)
				return true;
		return false;
	};
	assert(hasCategory("drift"));
	assert(hasCategory("security"));
	assert(hasCategory("concurrency"));
	assert(hasCategory("test_quality"));
	printf("Test 2 (drift/security/concurrency/test_quality "
	       "categories present): PASS\n");

	// ── Test 3: buildAll returns evidence from each new category ─
	auto all = builder.buildAll(pid);
	printf("buildAll returned %zu evidence(s)\n", all.size());
	assert(!all.empty());

	size_t drift_n = countByCategory(all, "drift");
	size_t security_n = countByCategory(all, "security");
	size_t concurrency_n = countByCategory(all, "concurrency");
	size_t test_quality_n = countByCategory(all, "test_quality");
	printf("  drift=%zu security=%zu concurrency=%zu "
	       "test_quality=%zu\n",
	       drift_n, security_n, concurrency_n, test_quality_n);

	assert(drift_n >= 1);
	assert(security_n >= 1);
	assert(concurrency_n >= 1);
	assert(test_quality_n >= 1);
	printf("Test 3 (each new category produced >= 1 evidence): "
	       "PASS\n");

	// ── Test 4: buildByCategory("concurrency") returns only
	// concurrency evidence, and at least 1 item.
	// ──────────────────────────────────────────────────────────
	auto conc_evs = builder.buildByCategory(pid, "concurrency");
	assert(!conc_evs.empty());
	for (const auto &ev : conc_evs)
		assert(ev.category == "concurrency");
	printf("Test 4 (buildByCategory concurrency, %zu evidence, "
	       "all category=concurrency): PASS\n",
	       conc_evs.size());

	// ── Test 5: verify concurrency evidence items reference the
	// expected mutex lock fact (symbol="m.Lock").
	// ──────────────────────────────────────────────────────────
	bool found_mutex = false;
	for (const auto &ev : conc_evs) {
		for (const auto &item : ev.items) {
			if (item.symbol == "m.Lock") {
				found_mutex = true;
				assert(item.file == "/src/lock_leak.go");
				assert(item.line == 5);
			}
		}
	}
	assert(found_mutex);
	printf("Test 5 (concurrency evidence contains m.Lock at "
	       "/src/lock_leak.go:5): PASS\n");

	// ── Test 6: buildByCategory for each new category returns
	// only matching evidence (no cross-contamination).
	// ──────────────────────────────────────────────────────────
	for (const auto &cat :
	     { std::string("drift"), std::string("security"),
	       std::string("concurrency"), std::string("test_quality") }) {
		auto evs = builder.buildByCategory(pid, cat);
		for (const auto &ev : evs)
			assert(ev.category == cat);
		assert(!evs.empty());
	}
	printf("Test 6 (buildByCategory isolates each new category): "
	       "PASS\n");

	// ── Test 7: buildByRule for known rules returns evidence ────
	for (const char *rule_name :
	     { "dead_pattern_todo", "unsafe_code_risk", "unchecked_ffi",
	       "mutex_without_unlock", "unwrap_in_non_test",
	       "todo_accumulation" }) {
		auto evs = builder.buildByRule(pid, rule_name);
		assert(!evs.empty());
	}
	printf("Test 7 (buildByRule for each new rule returns "
	       "evidence): PASS\n");

	// ── Test 8: buildByRule for unknown rule returns 0 ──────────
	{
		auto evs = builder.buildByRule(pid, "nonexistent_rule_xyz");
		assert(evs.empty());
	}
	printf("Test 8 (buildByRule unknown, 0 evidence): PASS\n");

	store.close();
	unlink(kDbPath);
	printf("\nAll domain_rules tests passed.\n");
	return 0;
}
