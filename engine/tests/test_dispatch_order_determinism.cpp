// test_dispatch_order_determinism — the resolved graph must not depend on the
// order the records happen to sit in the database.
//
// Defect guarded: Step 8.1c resolves a field-chain receiver ("h.registry") by
// walking the candidate types of its first segment through the global struct
// field table, and the FIRST type whose chain resolves wins. Those candidate
// types are loaded per variable NAME — the same name is declared with different
// types in different files — and the loader had no ORDER BY, so "first" meant
// "whichever row the scan reached first", i.e. semantic_records.rowid order.
// That order is not a property of the records: the parallel scheduler
// re-assigns rowids when it merges the per-module worker databases, so it
// changes with the worker partitioning. Measured on goagent, with
// byte-identical entities, references and semantic_records: `-w 2`/`-w 4`
// resolved 7283 edges (684 of them `dispatch`) and `-w 6` resolved 7295 (696) —
// six call sites that the ambiguity gate abstained on in one ordering and that
// dispatch expanded in the other (`freeCapableAgents` → `Manager.Get` /
// `QueueRegistry.Get`).
//
// The fixture makes the order decisive in one project:
//   iface.go     Getter (interface, method Get) + Registry.Get implements it
//   holder_a.go  HolderA.registry is a Getter      → the walk resolves the
//                receiver to an INTERFACE → dispatch expansion to Registry.Get
//   holder_b.go  HolderB.registry is a *Registry   → the walk resolves it to a
//                CONCRETE type → the same call gets no dispatch edge
// Both files declare a parameter named `h`, so the walk's candidate list for
// `h` is {HolderA, HolderB} and its order decides which of the two outcomes the
// call site gets. The test indexes the fixture, then REORDERS those two rows
// (the same project, same records, rowids swapped — what a different module
// packing does to them) and re-runs the resolver: the graph must not move.
//
// The reorder is proven to be effective three ways: the row order is read back
// and asserted different, a marker edge injected before the rebuild must be
// gone after it (so a skipped rebuild cannot make the comparison vacuous), and
// the expected dispatch edge is pinned explicitly.
#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

#include "test_check.h"
#include "test_engine_handle.h"

namespace
{

/// Write `content` to `path`, failing the test if the file cannot be created.
void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

/// Run `sql`, expecting success.
void execSql(sqlite3 *db, const char *sql)
{
	char *err = nullptr;
	const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL: sqlite3_exec: %s\n  %s\n", err ? err : "?",
			sql);
		if (err)
			sqlite3_free(err);
	}
	CHECK(rc == SQLITE_OK);
}

/// Every resolved call edge as one canonical line each — caller, callee and the
/// resolution kind, which is what differs between the two outcomes above.
std::string edgeFingerprint(sqlite3 *db)
{
	const char *sql =
		"SELECT se.name || '>' || te.name || '|' || r.resolution_kind "
		"FROM relation r "
		"JOIN entity se ON se.id = r.source_id "
		"JOIN entity te ON te.id = r.target_id "
		"WHERE r.type = 1 ORDER BY 1";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	std::string out;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s)
			out += std::string(s) + "\n";
	}
	sqlite3_finalize(st);
	return out;
}

/// Type of the FIRST record named `name` (kind is passed in so the fixture's
/// variable rows can be told apart from anything else).
std::string firstTypeOf(sqlite3 *db, const char *name, int kind)
{
	sqlite3_stmt *st = nullptr;
	const char *sql = "SELECT type_name FROM semantic_records "
			  "WHERE name=? AND kind=? ORDER BY rowid";
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 2, kind);
	std::string out;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		out = s ? s : "";
	}
	sqlite3_finalize(st);
	return out;
}

} // namespace

int main()
{
	const std::string proj_dir = "/tmp/dispatch_order_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir + "/src");

	writeFile(proj_dir + "/src/iface.go",
		  "package p\n"
		  "\n"
		  "type Getter interface {\n"
		  "\tGet(id string) error\n"
		  "}\n"
		  "\n"
		  "type Registry struct{}\n"
		  "\n"
		  "func (r *Registry) Get(id string) error { return nil }\n");
	writeFile(proj_dir + "/src/holders.go",
		  "package p\n"
		  "\n"
		  "type HolderA struct {\n"
		  "\tregistry Getter\n"
		  "}\n"
		  "\n"
		  "type HolderB struct {\n"
		  "\tregistry *Registry\n"
		  "}\n");
	// The call sites sit in files of their own: the Visitor resolves field
	// types per file, so `h.registry` is only resolvable through the global
	// struct-field table (Step 8.1c) when the struct is declared elsewhere.
	writeFile(proj_dir + "/src/use_a.go",
		  "package p\n"
		  "\n"
		  "func useA(h HolderA, id string) error {\n"
		  "\treturn h.registry.Get(id)\n"
		  "}\n");
	writeFile(proj_dir + "/src/use_b.go",
		  "package p\n"
		  "\n"
		  "func useB(h HolderB, id string) error {\n"
		  "\treturn h.registry.Get(id)\n"
		  "}\n");

	const std::string db_path = proj_dir + "/order.db";
	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());

	g_engine = engine_create(db_path.c_str());
	CHECK(g_engine != nullptr);
	const uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
						   "dispatch-order");
	CHECK(pid > 0);

	const std::string request = "{\"paths\":[\"" + proj_dir +
				    "/src/iface.go\",\"" + proj_dir +
				    "/src/holders.go\",\"" + proj_dir +
				    "/src/use_a.go\",\"" + proj_dir +
				    "/src/use_b.go\"]}";
	char *res = engine_index_files(g_engine, pid, request.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	// engine_destroy() joins the async knowledge builder before closing the
	// store (engine_lifecycle.cpp), so reading the database directly afterwards
	// is safe; every table asserted below is written synchronously by the index
	// path anyway.
	engine_destroy(g_engine);
	g_engine = nullptr;

	sqlite3 *db = nullptr;
	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);

	// ── The fixture has to resolve something ──────────────────────
	// `h` is declared as HolderA in one file and HolderB in the other, so the
	// type table holds both spellings for one name: that is the ambiguity the
	// walk used to settle by row order.
	const std::string before_type = firstTypeOf(db, "h", 17);
	CHECK(before_type == "HolderA" || before_type == "HolderB");
	const std::string before = edgeFingerprint(db);
	// Which order the parse workers happened to write is not the point (and is
	// itself what makes the graph flaky); the fixture has to have resolved
	// SOMETHING, or the comparison below is vacuous.
	CHECK(!before.empty());

	// ── Reorder the records ───────────────────────────────────────
	// Negating every rowid reverses the order a scan sees, leaving the records
	// themselves untouched — the same effect a different module packing has on
	// the merged database. The whole table, not just the two `h` rows: the
	// loaders read through a self-join, so which table drives the plan decides
	// whose order reaches the candidate vectors.
	execSql(db, "UPDATE semantic_records SET rowid = -rowid;");
	const std::string after_type = firstTypeOf(db, "h", 17);
	CHECK(after_type != before_type); // the lever really moved the order

	// A marker edge that the rebuild must remove, so a skipped rebuild cannot
	// make the comparison below pass for the wrong reason.
	execSql(db, "INSERT INTO relation (project_id, source_id, target_id, type, "
		    "confidence, resolver, resolution_kind, reason) "
		    "SELECT 1, (SELECT id FROM entity WHERE name='useA' LIMIT 1), "
		    "(SELECT id FROM entity WHERE name='Get' LIMIT 1), 1, "
		    "0.1, 'test', 'probe-kind', 'probe';");
	execSql(db, "DELETE FROM project_readiness;");
	sqlite3_close(db);

	// ── Re-run the resolver on the reordered records ──────────────
	g_engine = engine_create(db_path.c_str());
	CHECK(g_engine != nullptr);
	char *enh = engine_enhance_project(g_engine, pid);
	CHECK(enh != nullptr);
	CHECK(strstr(enh, "\"ok\":true") != nullptr);
	engine_free_string(enh);
	engine_destroy(g_engine);
	g_engine = nullptr;

	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
	// The rebuild ran (the marker is gone) and the graph did not move.
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db,
				 "SELECT COUNT(*) FROM relation "
				 "WHERE resolution_kind='probe'",
				 -1, &st, nullptr) == SQLITE_OK);
	CHECK(sqlite3_step(st) == SQLITE_ROW);
	const long long probe_left = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	CHECK(probe_left == 0);

	const std::string after = edgeFingerprint(db);
	// The pinned outcome: with the candidate types ordered by name the walk
	// resolves `h` to HolderA, whose `registry` is a Getter, so both call sites
	// expand to the single implementation instead of falling to the ambiguity
	// gate. Pinning it keeps the equality check below from passing on two
	// equally empty graphs.
	CHECK(after.find("useA>Get|dispatch") != std::string::npos);
	CHECK(after.find("useB>Get|dispatch") != std::string::npos);
	if (before != after) {
		fprintf(stderr,
			"FAIL: the resolved edges follow the record order.\n"
			"  before reorder (first `h` type = %s):\n%s"
			"  after reorder  (first `h` type = %s):\n%s",
			before_type.c_str(), before.c_str(), after_type.c_str(),
			after.c_str());
	}
	CHECK(before == after);

	sqlite3_close(db);
	unlink(db_path.c_str());
	if (checkFailures()) {
		fprintf(stderr, "=== dispatch order determinism: FAILED ===\n");
		return 1;
	}
	printf("=== dispatch order determinism test passed ===\n");
	return 0;
}
