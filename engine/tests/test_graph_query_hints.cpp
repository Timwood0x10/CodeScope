// test_graph_query_hints.cpp
//
// Pins three surfaces that only ever failed silently:
//
// 1. query::bareNameCandidates — the shared homonym probe used by
//    trace_flow / codescope_trace. Must return "" for 0/1 matches (normal
//    tracing proceeds) and a `{"ambiguous":true,"candidates":[…]}` fragment
//    for 2+ (T5 finding #9).
// 2. graph_query's empty-result hint. A typed name that exists under a
//    different entity kind must produce exactly ONE "hint" key (two probes
//    must not emit duplicate JSON object names), and a node written without
//    a type label (`:name`) must not hint at all — there the type filter is
//    not why the result is empty (T5 finding #14 follow-up).
// 3. detect_ffi_boundaries.external_symbols — called-but-undefined symbols,
//    i.e. one-sided FFI prototypes like `int rust_add(int,int);` that the
//    name-prefix list cannot see (T5 finding #7).
//
// The database state is built with explicit SQL rather than by indexing a
// fixture, so each case tests exactly what it claims to.

#include "../include/engine.h"
#include "../src/query/graph_query.h"
#include "../src/query/query_engine.h"
#include "../src/store/store.h"

#include "test_check.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace store;

static const char *kDbPath = "/tmp/test_graph_query_hints.db";

/// Run a statement, failing loudly: this test builds its own state, so an
/// error here means the fixture is wrong rather than the code under test.
static void execOrDie(sqlite3 *db, const std::string &sql)
{
	char *err = nullptr;
	if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) !=
	    SQLITE_OK) {
		fprintf(stderr, "FAIL: sqlite3_exec: %s | sql=%s\n",
			err ? err : "(no message)", sql.c_str());
		sqlite3_free(err);
		exit(1);
	}
}

static void insertEntity(sqlite3 *db, uint64_t pid, int64_t id, int kind,
			 const std::string &name, const std::string &file,
			 int start_row)
{
	execOrDie(db,
		  "INSERT INTO entity (id, project_id, kind, name, "
		  "qualified_name, file_path, language, start_row, start_col, "
		  "end_row, end_col) VALUES (" +
			  std::to_string(id) + "," + std::to_string(pid) + "," +
			  std::to_string(kind) + ",'" + name + "','','" + file +
			  "','c'," + std::to_string(start_row) + ",0," +
			  std::to_string(start_row + 1) + ",0)");
}

static void insertCallRelation(sqlite3 *db, uint64_t pid, int64_t src,
			       int64_t tgt)
{
	execOrDie(
		db,
		"INSERT INTO relation (project_id, source_id, target_id, type) "
		"VALUES (" +
			std::to_string(pid) + "," + std::to_string(src) + "," +
			std::to_string(tgt) + ",1)");
}

/// A CallExpr semantic record — the shape detect_ffi_boundaries reads to
/// find symbols that are called but have no definition entity.
static void insertCallExpr(sqlite3 *db, uint64_t pid, int64_t id,
			   const std::string &name, const std::string &file,
			   int start_row)
{
	execOrDie(db, "INSERT INTO semantic_records (original_id, project_id, "
		      "kind, name, file_path, language, start_row, end_row) "
		      "VALUES (" +
			      std::to_string(id) + "," + std::to_string(pid) +
			      ",9,'" + name + "','" + file + "','c'," +
			      std::to_string(start_row) + "," +
			      std::to_string(start_row) + ")");
}

/// Count occurrences of `"key":` in a JSON document — used to assert a key
/// appears exactly once, which a JSON parse alone cannot check (duplicate
/// object names are accepted by lenient parsers and the last one wins).
static int countKey(const std::string &json, const char *key)
{
	std::string needle = std::string("\"") + key + "\":";
	int n = 0;
	size_t pos = 0;
	while ((pos = json.find(needle, pos)) != std::string::npos) {
		++n;
		pos += needle.size();
	}
	return n;
}

static bool contains(const std::string &hay, const char *needle)
{
	return hay.find(needle) != std::string::npos;
}

int main()
{
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	const uint64_t pid = store.createProject("/tmp", "graph-query-hints");
	CHECK(pid > 0);
	sqlite3 *db = store.handle();
	CHECK(db != nullptr);

	// Fixture:
	//   1   Widget  kind=2 (class)          src/widget.h
	//   2   Widget  kind=0 (constructor)    src/widget.c
	//   3   gadget  kind=0                  src/widget.c
	//   4   other   kind=0                  src/other.c
	// 1 -> 4 and 3 -> 4 are real Calls edges.
	insertEntity(db, pid, 1, 2, "Widget", "src/widget.h", 10);
	insertEntity(db, pid, 2, 0, "Widget", "src/widget.c", 20);
	insertEntity(db, pid, 3, 0, "gadget", "src/widget.c", 30);
	insertEntity(db, pid, 4, 0, "other", "src/other.c", 5);
	insertCallRelation(db, pid, 1, 4);
	insertCallRelation(db, pid, 3, 4);
	// One-sided FFI: `extern_add` is called but never defined here.
	insertCallExpr(db, pid, 100, "extern_add", "src/widget.c", 40);

	// ── Case 1: bareNameCandidates probe ────────────────────────
	{
		std::string none =
			query::bareNameCandidates(db, pid, "nonexistent");
		std::string one = query::bareNameCandidates(db, pid, "gadget");
		CHECK(none.empty() &&
		       "0 matches must probe as unambiguous (caller reports "
		       "not-found)");
		CHECK(one.empty() &&
		       "1 match must probe as unambiguous (caller traces "
		       "it)");
		printf("Test 1 (0/1 match -> empty probe): PASS\n");
	}

	// ── Case 2: 2 matches -> ambiguous + candidates ─────────────
	{
		std::string amb = query::bareNameCandidates(db, pid, "Widget");
		CHECK(contains(amb, "\"ambiguous\":true"));
		CHECK(countKey(amb, "graph_node_id") == 2);
		CHECK(contains(amb, "widget.h") && contains(amb, "widget.c"));
		printf("Test 2 (2 matches -> ambiguous, 2 candidates): PASS\n");
	}

	// ── Case 3: typed mismatch -> exactly one hint ──────────────
	// `gadget` exists only as kind 0, so `Class:gadget` resolves to
	// nothing — the type filter is why the result is empty and the hint
	// must fire once and name the kind it did find.
	{
		std::string q = query::executeGraphQuery(
			pid,
			"MATCH (Class:gadget)-[Calls]->(Function:other) "
			"RETURN Class.name",
			&store);
		CHECK(q.find("\"total\":0") != std::string::npos);
		CHECK(countKey(q, "hint") == 1 &&
		       "a kind mismatch must emit exactly one hint key");
		CHECK(contains(q, "kind 0"));
		printf("Test 3 (typed mismatch -> single hint): PASS\n");
	}

	// ── Case 4: both sides mismatch -> still one hint key ───────
	{
		std::string q = query::executeGraphQuery(
			pid,
			"MATCH (Class:gadget)-[Calls]->(Class:other) "
			"RETURN Class.name",
			&store);
		CHECK(q.find("\"total\":0") != std::string::npos);
		CHECK(countKey(q, "hint") == 1 &&
		       "two probes must merge into one hint key, never two");
		printf("Test 4 (both-sides mismatch -> one hint key): PASS\n");
	}

	// ── Case 5: untyped node (:name) -> no hint at all ──────────
	// `:gadget` parses to name with an EMPTY type, so the type filter
	// is not why the result is empty — hinting would be misleading.
	{
		std::string q = query::executeGraphQuery(
			pid,
			"MATCH (:gadget)-[Calls]->(:nosuch) RETURN gadget.name",
			&store);
		CHECK(q.find("\"total\":0") != std::string::npos);
		CHECK(countKey(q, "hint") == 0 &&
		       "an untyped node must not produce a kind-mismatch "
		       "hint");
		printf("Test 5 (untyped node -> no hint): PASS\n");
	}

	// ── Case 6: a query that matches -> no hint ─────────────────
	{
		std::string q = query::executeGraphQuery(
			pid,
			"MATCH (Function:gadget)-[Calls]->(Function:other) "
			"RETURN Function.name",
			&store);
		CHECK(q.find("\"total\":1") != std::string::npos);
		CHECK(countKey(q, "hint") == 0 &&
		       "a matching query must not carry a hint");
		printf("Test 6 (matching query -> no hint): PASS\n");
	}

	// ── Test 8: LIMIT is honoured, unknown trailing text is rejected ──
	// Regression: the DSL documented `LIMIT n` (the README's graph-query
	// benchmarks instruct it on large graphs) and the parser ignored it — plus
	// every other trailing token — silently, so `LIMIT 10` returned the whole
	// result set and the MCP transport then replaced the oversized response
	// with an error the caller could not act on.
	{
		insertEntity(db, pid, 9101, 0, "lq_caller", "/t/lq.c", 10);
		for (int i = 0; i < 5; ++i)
			insertEntity(db, pid, 9110 + i, 0,
				     "lq_callee" + std::to_string(i), "/t/lq.c",
				     20 + i);
		for (int i = 0; i < 5; ++i)
			insertCallRelation(db, pid, 9101, 9110 + i);

		const std::string base =
			"MATCH (Function:lq_caller)-[Calls]->(Function)";

		const std::string all =
			query::executeGraphQuery(pid, base.c_str(), &store);
		CHECK(countKey(all, "source") == 5);
		CHECK(!contains(all, "\"truncated\""));

		const std::string two = query::executeGraphQuery(
			pid, (base + " LIMIT 2").c_str(), &store);
		CHECK(countKey(two, "source") == 2);
		CHECK(contains(two, "\"total\":2"));
		CHECK(contains(two, "\"truncated\":true"));

		// Keywords are case-insensitive, like MATCH.
		const std::string three = query::executeGraphQuery(
			pid, (base + " limit 3").c_str(), &store);
		CHECK(countKey(three, "source") == 3);
		CHECK(contains(three, "\"truncated\":true"));

		// A limit above the match count is not a truncation.
		const std::string big = query::executeGraphQuery(
			pid, (base + " LIMIT 99").c_str(), &store);
		CHECK(countKey(big, "source") == 5);
		CHECK(!contains(big, "\"truncated\""));

		// `RETURN` stays accepted (the DSL's documented surface, used by the
		// tests above) and may follow LIMIT.
		const std::string ret = query::executeGraphQuery(
			pid, (base + " LIMIT 2 RETURN Function.name").c_str(),
			&store);
		CHECK(countKey(ret, "source") == 2);
		CHECK(contains(ret, "\"truncated\":true"));

		// A malformed clause — and any other trailing text — is an error,
		// never a silent no-op.
		for (const char *bad : { " LIMIT 0", " LIMIT abc", " LIMIT",
					 " GARBAGE", " LIMIT 1 LIMIT 2" }) {
			const std::string r = query::executeGraphQuery(
				pid, (base + bad).c_str(), &store);
			CHECK(contains(r, "\"error\""));
			CHECK(countKey(r, "source") == 0);
		}
		printf("Test 8 (LIMIT honoured, unknown trailing text rejected): "
		       "PASS\n");
	}

	// ── Case 7: one-sided FFI appears as external_symbols ───────
	// engine_detect_ffi_boundaries runs against the GLOBAL FFI store,
	// not a local GraphStore, so this case drives the real
	// engine_init / engine_create_project lifecycle and seeds its DB
	// through a second sqlite3 connection.
	{
		store.close();
		unlink(kDbPath);

		if (engine_init(kDbPath) != 0) {
			fprintf(stderr, "FAIL: engine_init\n");
			return 1;
		}
		const uint64_t fpid =
			engine_create_project("/tmp", "ffi-one-sided");
		CHECK(fpid > 0);

		sqlite3 *fdb = nullptr;
		if (sqlite3_open(kDbPath, &fdb) != SQLITE_OK) {
			fprintf(stderr, "FAIL: sqlite3_open for fixture: %s\n",
				sqlite3_errmsg(fdb));
			engine_shutdown();
			return 1;
		}
		insertEntity(fdb, fpid, 1, 0, "run_c_side", "src/main.c", 1);
		insertCallExpr(fdb, fpid, 100, "extern_add", "src/main.c", 40);
		insertCallExpr(fdb, fpid, 101, "run_c_side", "src/main.c", 10);
		sqlite3_close(fdb);

		char *raw = engine_detect_ffi_boundaries(fpid);
		CHECK(raw != nullptr);
		std::string s(raw);
		engine_free_string(raw);
		engine_shutdown();

		CHECK(contains(s, "\"external_symbols\":["));
		CHECK(contains(s, "extern_add"));
		size_t ext = s.find("\"external_symbols\":");
		size_t orphan = s.find("\"orphan_symbols\":");
		CHECK(ext != std::string::npos &&
		       orphan != std::string::npos && ext < orphan);
		std::string ext_block = s.substr(ext, orphan - ext);
		// `run_c_side` is defined here (entity kind=0), so the CallExpr
		// to it must NOT be reported as external.
		CHECK(contains(ext_block, "extern_add"));
		CHECK(!contains(ext_block, "run_c_side"));
		printf("Test 7 (one-sided FFI -> external_symbols): PASS\n");
	}

	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	printf("\n=== test_graph_query_hints PASSED ===\n");
	printf("Homonym probe, single-hint contract, untyped-node quiet, "
	       "external_symbols, LIMIT\n");
	return checkFailures() ? 1 : 0;
}
