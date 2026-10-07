// test_capability_state_matches_verifier — the two capability views must agree.
//
// Defect guarded: build_project_state reports project_state.capability.total /
// verified from `capability_state` (model::StateBuilder::buildCapabilityState),
// while verify_claim / detect_capability_drift report the verdict of
// verify::CapabilityVerifier for the same DECLARED capabilities. The two used
// to answer with different rules — a naming heuristic with no call-graph
// evidence in the builder, a name match plus a caller/export requirement in the
// verifier — so one view called a capability Implemented while the other
// Contradicted it (measured on goagent: 19 vs 7; on this repository: 3 vs 3
// contradicted).
//
// The rule now lives in verify::implementingEntitiesFor and the builder
// restates it in SQL (model/ cannot include verify/). Restating is only safe
// while the two stay equivalent, so this test is the equivalence check: for
// every declared capability it compares
//
//     capability_state.state  ==  implementingEntitiesFor(...).empty()
//                                 ? "Declared" : "Implemented"
//
// and pins the cases where a hand-written restatement drifts: the arms that
// match two normalised spellings carry their own length floors, and a floor
// applied to the wrong name ("c_ac" → "cac") puts the builder back on the
// accidental-prefix path the floor exists to block.
//
// Exit code: 0 when every check passed, 1 otherwise.

#include "../src/model/state_builder.h"
#include "../src/store/store.h"
#include "../src/verify/capability_verifier.h"

#include "test_check.h"

#include <sqlite3.h>
#include <string>
#include <unistd.h>

/// Insert a declared capability (the `capability` table, normally filled from
/// the project README by the knowledge builder).
static void insertCapability(store::GraphStore &store, uint64_t project_id,
			     const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO capability (project_id, name, source_kind) "
			  "VALUES (?,?, 'readme')";
	sqlite3_stmt *stmt = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a function entity. visibility=1 marks an exported/public entry point,
/// which counts as evidence on its own (an FFI export's callers live in the
/// other language and produce no edge here).
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name, int visibility = 0)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col, visibility) "
			  "VALUES (?,?,0,?,'','/src/a.cpp','cpp',0,0,0,0,?)";
	sqlite3_stmt *stmt = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, visibility);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a call edge (relation type=1) from source_id to target_id — the
/// caller evidence both views require.
static void insertCall(store::GraphStore &store, uint64_t project_id,
		       int64_t source_id, int64_t target_id)
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO relation (project_id, source_id, "
			  "target_id, type) VALUES (?,?,?,1)";
	sqlite3_stmt *stmt = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, source_id);
	sqlite3_bind_int64(stmt, 3, target_id);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// The state the builder wrote for one capability ("Implemented" /
/// "Declared"); "(missing)" when it wrote no row, so a failure names the gap
/// instead of comparing against an empty string.
static std::string stateOf(store::GraphStore &store, uint64_t project_id,
			   const char *name)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT state FROM capability_state "
			  "WHERE project_id=? AND name=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return "(prepare failed)";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	std::string out = "(missing)";
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		out = s ? s : "";
	}
	sqlite3_finalize(stmt);
	return out;
}

/// Scalar helper for the row-count assertions.
static int64_t scalar(store::GraphStore &store, const char *sql,
		      uint64_t project_id)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t n = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}

/// The one assertion this test exists for: the builder's row and the
/// verifier's rule must describe the same capability the same way.
static void expectAgreement(store::GraphStore &store, uint64_t project_id,
			    const char *capability)
{
	bool ok = false;
	const std::vector<int64_t> ids =
		verify::implementingEntitiesFor(&store, project_id, capability,
						ok);
	CHECK(ok);
	const std::string want = ids.empty() ? "Declared" : "Implemented";
	const std::string got = stateOf(store, project_id, capability);
	if (want != got) {
		fprintf(stderr,
			"FAIL: \"%s\" — capability_state said \"%s\" while "
			"implementingEntitiesFor returned %zu entities "
			"(\"%s\")\n",
			capability, got.c_str(), ids.size(), want.c_str());
	}
	CHECK(want == got);
}

int main()
{
	const char *db_path = "/tmp/codescope_test_cap_state_agrees.db";
	unlink(db_path);

	store::GraphStore store;
	CHECK(store.open(db_path));
	const uint64_t project_id = store.createProject("/test", "cap_state");
	CHECK(project_id > 0);

	// ── Declared capabilities ──────────────────────────────────────
	// "IncrementalIndexing": legit bidirectional prefix of the code symbol
	// "IncrementalIndex" — must stay Implemented.
	// "Run": exact match at a length below the floor — must stay
	// Implemented, because only the accidental PREFIX is rejected.
	// "CacheIndex": its only near-match is "c_ac", whose normalised name is
	// "cac" (3 characters). The de-underscore arm must floor the NORMALISED
	// entity name, otherwise 'cacheindex' LIKE 'cac%' makes the builder
	// report Implemented with no implementing entity at all.
	insertCapability(store, project_id, "IncrementalIndexing");
	insertCapability(store, project_id, "Run");
	insertCapability(store, project_id, "CacheIndex");
	insertCapability(store, project_id, "TelemetryExport");

	// A caller so every target below has the evidence both views require.
	insertEntity(store, project_id, 1, "main");
	insertEntity(store, project_id, 10, "IncrementalIndex");
	insertCall(store, project_id, 1, 10);
	insertEntity(store, project_id, 11, "Run");
	insertCall(store, project_id, 1, 11);
	insertEntity(store, project_id, 12, "c_ac");
	insertCall(store, project_id, 1, 12);
	// Name-matches "TelemetryExport" by prefix, has no caller and is not
	// exported: a name match without evidence is not an implementation.
	insertEntity(store, project_id, 13, "TelemetryExporter");

	model::StateBuilder builder(&store, project_id);
	CHECK(builder.buildCapabilityState() == 4);

	// 1. The snapshot mirrors the DECLARED set — no heuristic extra rows.
	CHECK(scalar(store,
		     "SELECT COUNT(*) FROM capability_state WHERE project_id=?",
		     project_id) == 4);

	// 2. Every row agrees with the verifier's rule.
	expectAgreement(store, project_id, "IncrementalIndexing");
	expectAgreement(store, project_id, "Run");
	expectAgreement(store, project_id, "CacheIndex");
	expectAgreement(store, project_id, "TelemetryExport");

	// 3. Pinned verdicts, so the agreement above cannot pass vacuously by
	// both sides going blank.
	CHECK(stateOf(store, project_id, "IncrementalIndexing") == "Implemented");
	CHECK(stateOf(store, project_id, "Run") == "Implemented");
	CHECK(stateOf(store, project_id, "CacheIndex") == "Declared");
	CHECK(stateOf(store, project_id, "TelemetryExport") == "Declared");

	// 4. A second build replaces the rows instead of appending a copy of
	// every one (capability_state has no UNIQUE(project_id, name), so the
	// old INSERT OR IGNORE had no conflict target).
	CHECK(builder.buildCapabilityState() == 4);
	CHECK(scalar(store,
		     "SELECT COUNT(*) FROM capability_state WHERE project_id=?",
		     project_id) == 4);

	store.close();
	unlink(db_path);

	if (checkFailures()) {
		fprintf(stderr, "=== capability_state/verifier agreement: FAILED ===\n");
		return 1;
	}
	printf("=== capability_state/verifier agreement test passed ===\n");
	return 0;
}
