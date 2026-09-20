// test_resolution_kind: `relation.resolution_kind` must name the evidence that
// DECIDED the match, not the first non-empty reference field.
//
// The kind used to be read off "which reference field is non-empty"
// (receiver_type > qualified_target > import_alias > name), which answers a
// different question: a call carrying a `receiver_type` field but resolved by
// name/namespace evidence — the receiver evidence contributing nothing to the
// winning score — was still labelled `receiver_type`. Per-kind accuracy audits
// group by this column, so all of them inherited that skew.
//
// The label now comes from the winning candidate's largest positive factor
// contribution (Candidate::deciding_factor, recorded by applyConstraints while
// it accumulates the weighted score), with an exact qualified-name hit checked
// explicitly because it is not a scoring factor. This test asserts both
// directions: the label is NOT `receiver_type` when the receiver evidence did
// not decide the match, and IS `receiver_type` when it did.

#include "../src/resolver/pipeline.h"
#include "../src/store/store.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

using namespace resolver;

static const char *kDbPath = "/tmp/test_resolution_kind.db";

/// Insert an entity row. `qualified_name` carries the receiver evidence a
/// method call can match against ("Box.Beta" for receiver_type "Box").
static void insertEntity(store::GraphStore &store, uint64_t project_id,
			 int64_t id, const char *name,
			 const char *qualified_name, const char *file_path,
			 const char *language = "cpp")
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO entity (id, project_id, kind, name, "
			  "qualified_name, file_path, language, start_row, "
			  "start_col, end_row, end_col) "
			  "VALUES (?,?,0,?,?,?,?,0,0,0,0)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, qualified_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, language, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a semantic_records InterfaceImpl row (kind=20): `impl` implements
/// `iface`. This is what pipeline_load.cpp reads to build
/// interface_impl_index_, i.e. what drives the dispatch expansion.
static void insertInterfaceImpl(store::GraphStore &store, uint64_t project_id,
				const char *impl, const char *iface)
{
	sqlite3 *db = store.handle();
	// original_id and file_path are NOT NULL in semantic_records; the dispatch
	// index reads only name/type_name, so they carry placeholder values here.
	const char *sql =
		"INSERT INTO semantic_records (project_id, original_id, kind, "
		"name, type_name, file_path) "
		"VALUES (?,1,20,?,?,'/fixture/iface_impl')";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, impl, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, iface, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// Insert a reference row: caller_id calls `name`, carrying `receiver_type`.
static void insertReference(store::GraphStore &store, uint64_t project_id,
			    int64_t caller_id, const char *name,
			    const char *receiver_type,
			    const char *qualified_target = "")
{
	sqlite3 *db = store.handle();
	const char *sql = "INSERT INTO reference (project_id, caller_id, name, "
			  "arity, call_kind, start_row, start_col, "
			  "receiver_type, call_site_file, qualified_target) "
			  "VALUES (?,?,?,0,0,0,0,?,?,?)";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, caller_id);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, receiver_type, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, "/src/app/main.cpp", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, qualified_target, -1, SQLITE_TRANSIENT);
	assert(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

/// The call edge targeting `target_id`: its resolution_kind and its reason.
static bool callEdgeTo(store::GraphStore &store, uint64_t project_id,
		       int64_t target_id, std::string &kind,
		       std::string &reason)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT resolution_kind, reason FROM relation "
			  "WHERE project_id=? AND type=1 AND target_id=?";
	sqlite3_stmt *stmt = nullptr;
	assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, target_id);
	bool found = false;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *k = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *r = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		kind = k ? k : "";
		reason = r ? r : "";
		found = true;
	}
	sqlite3_finalize(stmt);
	return found;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	uint64_t pid = store.createProject("/test", "test_resolution_kind");
	assert(pid > 0);

	// ── Case A: receiver evidence is present but does NOT decide ──
	// Caller and candidate 2 share a directory (namespace/module evidence),
	// candidate 3 does not. The reference carries receiver_type="Unrelated",
	// which matches neither candidate's qualified_name — so the receiver factor
	// contributes nothing decisive even though the field is non-empty.
	insertEntity(store, pid, 1, "main", "", "/src/app/main.cpp");
	insertEntity(store, pid, 2, "Alpha", "", "/src/app/other.cpp");
	insertEntity(store, pid, 3, "Alpha", "", "/src/other/other.cpp");
	insertReference(store, pid, 1, "Alpha", "Unrelated");

	// ── Case B: qualified_target present but NOT the evidence that decided ──
	// Same shape as Case A for a different field of the old priority chain:
	// the reference carries a qualified_target, but it matches neither
	// candidate's qualified_name, so the old rule's "qualified" label would be
	// just as invented.
	insertEntity(store, pid, 4, "Beta", "", "/src/app/beta.cpp");
	insertEntity(store, pid, 5, "Beta", "", "/src/other/beta.cpp");
	insertReference(store, pid, 1, "Beta", "", "Unrelated.Thing");

	// ── Case D: a fuzzy candidate is not labelled "exact_local" ───
	// The fast path runs whenever the candidate set holds one same-directory
	// entity — including when that set came from the fuzzy fallback, whose name
	// only matches by prefix ("DeltaThi" -> "DeltaThing"). The label said
	// "exact_local" regardless, i.e. it claimed an exactness the match never
	// had.
	insertEntity(store, pid, 6, "DeltaThing", "", "/src/app/delta.cpp");
	insertReference(store, pid, 1, "DeltaThi", "Whatever");

	// ── Case E: dispatch expansion must respect the language filter ──
	// "Iface" is implemented twice — once in C++ and once in Python — and the
	// call site is C++. The expansion selects targets by qualified_name prefix
	// only, so without a language filter the .py implementation became a CALLS
	// target of a .cpp call site.
	insertEntity(store, pid, 7, "Do", "CppImpl.Do",
		     "/src/app/cpp_impl.cpp");
	insertEntity(store, pid, 8, "Do", "PyImpl.Do", "/src/app/py_impl.py",
		     "python");
	insertInterfaceImpl(store, pid, "CppImpl", "Iface");
	insertInterfaceImpl(store, pid, "PyImpl", "Iface");
	insertReference(store, pid, 1, "Do", "Iface");

	// ── Case C: the receiver mapping itself ───────────────────────
	// ReceiverMatch is the one factor the audits care most about, and whether
	// it can ever be the largest contributor in a real fixture depends on the
	// weights, so its mapping is asserted directly.
	assert(resolutionKindFromFactor("ReceiverMatch") == "receiver_type");

	ResolverPipeline pipe(&store, pid);
	int64_t resolved = pipe.run();
	printf("resolved %lld call edge(s)\n", (long long)resolved);
	// Diagnostics: every edge the pipeline wrote, so a failure here says which
	// case abstained instead of only that something did.
	{
		sqlite3_stmt *st = nullptr;
		assert(sqlite3_prepare_v2(
			       store.handle(),
			       "SELECT target_id, confidence, "
			       "resolution_kind, reason FROM relation "
			       "WHERE project_id=? AND type=1",
			       -1, &st, nullptr) == SQLITE_OK);
		sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
		while (sqlite3_step(st) == SQLITE_ROW) {
			printf("  edge -> target=%lld conf=%.3f kind=%s reason=%s\n",
			       (long long)sqlite3_column_int64(st, 0),
			       sqlite3_column_double(st, 1),
			       reinterpret_cast<const char *>(
				       sqlite3_column_text(st, 2)),
			       reinterpret_cast<const char *>(
				       sqlite3_column_text(st, 3)));
		}
		sqlite3_finalize(st);
	}
	assert(resolved >= 2);

	// ── Case A assertions ─────────────────────────────────────────
	std::string kind_a;
	std::string reason_a;
	if (!callEdgeTo(store, pid, 2, kind_a, reason_a)) {
		fprintf(stderr,
			"FAIL: Case A did not resolve to the same-directory "
			"candidate (entity 2)\n");
		return 1;
	}
	printf("Case A: target=2 kind=%s reason=%s\n", kind_a.c_str(),
	       reason_a.c_str());
	// The regression: this edge used to be labelled `receiver_type` purely
	// because the reference's receiver_type field was non-empty.
	assert(kind_a != "receiver_type");
	// The label must name the evidence that decided it.
	assert(reason_a.find("decided_by=") != std::string::npos);

	// ── Case B assertions ─────────────────────────────────────────
	std::string kind_b;
	std::string reason_b;
	if (!callEdgeTo(store, pid, 4, kind_b, reason_b)) {
		fprintf(stderr,
			"FAIL: Case B did not resolve to the same-directory "
			"candidate (entity 4)\n");
		return 1;
	}
	printf("Case B: target=4 kind=%s reason=%s\n", kind_b.c_str(),
	       reason_b.c_str());
	assert(kind_b != "qualified");
	assert(reason_b.find("decided_by=") != std::string::npos);

	// ── Case D assertions ─────────────────────────────────────────
	std::string kind_d;
	std::string reason_d;
	if (!callEdgeTo(store, pid, 6, kind_d, reason_d)) {
		fprintf(stderr,
			"FAIL: Case D did not resolve the fuzzy candidate\n");
		return 1;
	}
	printf("Case D: target=6 kind=%s reason=%s\n", kind_d.c_str(),
	       reason_d.c_str());
	// The name matched by prefix, so "exact_local" would be a claim the match
	// never earned.
	assert(kind_d == "fuzzy_local");

	// ── Case E assertions ─────────────────────────────────────────
	std::string kind_e;
	std::string reason_e;
	if (!callEdgeTo(store, pid, 7, kind_e, reason_e)) {
		fprintf(stderr,
			"FAIL: Case E lost the same-language dispatch target\n");
		return 1;
	}
	printf("Case E: target=7 kind=%s reason=%s\n", kind_e.c_str(),
	       reason_e.c_str());
	assert(kind_e == "dispatch");
	// The Python implementation must not be a target of a C++ call site.
	std::string kind_py;
	std::string reason_py;
	assert(!callEdgeTo(store, pid, 8, kind_py, reason_py));

	store.close();
	unlink(kDbPath);
	printf("=== test_resolution_kind PASSED ===\n");
	return 0;
}
