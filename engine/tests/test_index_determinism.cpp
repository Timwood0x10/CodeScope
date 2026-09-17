// test_index_determinism.cpp — regression test for reproducible indexing.
//
// Defect guarded: buildGraph assigned entity ids with
// `ROW_NUMBER() OVER ()` over semantic_records, i.e. in the table's scan
// order — which is the row INSERT order and therefore depends on how the
// parse workers happened to finish. The same source tree indexed twice could
// produce different entity ids, and every downstream id (relation,
// type_ref, import, CSR adjacency) shifted with it. That broke
// reproducibility: snapshot comparisons, id-keyed regressions and any
// "index twice and diff" check were not stable.
//
// The test indexes one fixture into two independent databases and compares
// the full dumps:
//
//   * entities  — (file_path, kind, name, start_row, start_col) -> id
//   * relations — (source tuple) -> (target tuple), type
//
// Both dumps must be byte-identical, which requires the id assignment to be
// a pure function of the input.
//
// To make the run reliably exercise the defect, the two runs use different
// parse-worker counts (1 and 8). The worker count decides the order in which
// settled files reach SQLite, i.e. the row order of semantic_records that
// `ROW_NUMBER() OVER ()` used to follow. Correct behaviour is that the
// worker count changes the speed but never the result.
//
// Uses the explicit check() pattern (the engine build defines NDEBUG).
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../include/engine.h"
#include "../src/ir/semantic_unit.h"
#include "../src/store/store.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <sqlite3.h>
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

/// Number of fixture translation units. Several files are required: the
/// row order in semantic_records depends on which parse worker finishes
/// first, so one file would not exercise the id-assignment order.
static const int kFixtureFiles = 6;

/// Functions emitted per fixture file (helper, compute, entry, extra).
static const int kFuncsPerFile = 4;

/// Lower bound on the entities/relations the fixture must produce, so an
/// empty (or failed) index cannot make the comparison pass vacuously.
static const long long kMinEntities = kFixtureFiles * kFuncsPerFile;
static const long long kMinRelations = 10;

static void writeFile(const std::string &path, const std::string &content)
{
	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path());
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen fixture");
	fputs(content.c_str(), f);
	fclose(f);
}

/// Create the fixture tree: kFixtureFiles directories, each with one
/// translation unit declaring kFuncsPerFile functions that call each other.
static void buildFixture(const std::string &root)
{
	std::filesystem::remove_all(root);
	for (int f = 0; f < kFixtureFiles; ++f) {
		const std::string idx = std::to_string(f);
		std::string body;
		body += "namespace ns" + idx + " {\n";
		body += "int helper" + idx + "(int x) { return x + " + idx +
			"; }\n";
		body += "int compute" + idx + "(int x) { return helper" + idx +
			"(x) * 2; }\n";
		body += "int entry" + idx + "(int x) { return compute" + idx +
			"(x); }\n";
		body += "int extra" + idx + "(int x) { return compute" + idx +
			"(x) + 1; }\n";
		body += "} // namespace ns" + idx + "\n";
		writeFile(root + "/pkg" + idx + "/unit" + idx + ".cpp", body);
	}
}

/// Index `dir` into a fresh database using `workers` parse workers, then
/// shut the engine down so the on-disk state is final before inspection.
///
/// The worker count is deliberately part of the test: it determines the
/// order in which settled files reach SQLite. Entity ids must not depend
/// on that order.
static uint64_t indexInto(const std::string &dir, const char *db_path,
			  int workers)
{
	// setenv() happens before any engine thread exists (the previous run
	// was fully shut down), so it is not a concurrent write to environ.
	std::string w = std::to_string(workers);
	setenv("CODESCOPE_WORKERS", w.c_str(), 1);

	unlink(db_path);
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(dir.c_str(), "determinism");
	check(pid > 0, "create_project");
	char *idx = engine_index_project(pid, dir.c_str(), nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);
	engine_shutdown();

	unsetenv("CODESCOPE_WORKERS");
	return pid;
}

/// Read one TEXT column, mapping NULL to the empty string.
static std::string col(sqlite3_stmt *st, int i)
{
	const char *t =
		reinterpret_cast<const char *>(sqlite3_column_text(st, i));
	return t ? t : "";
}

/// Dump every entity as "file|kind|name|row|col|id", ordered by the
/// semantic key so only the id can make the two dumps differ.
static std::vector<std::string> dumpEntities(const char *db_path,
					     uint64_t project_id)
{
	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open entities");
	const char *sql =
		"SELECT file_path, kind, name, start_row, start_col, "
		"id FROM entity WHERE project_id=? "
		"ORDER BY file_path, kind, name, start_row, start_col";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare dumpEntities");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::vector<std::string> out;
	while (sqlite3_step(st) == SQLITE_ROW) {
		out.push_back(col(st, 0) + "|" +
			      std::to_string(sqlite3_column_int(st, 1)) + "|" +
			      col(st, 2) + "|" +
			      std::to_string(sqlite3_column_int(st, 3)) + "|" +
			      std::to_string(sqlite3_column_int(st, 4)) + "|" +
			      std::to_string(sqlite3_column_int64(st, 5)));
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	return out;
}

/// Dump every relation as source tuple -> target tuple with its type.
static std::vector<std::string> dumpRelations(const char *db_path,
					      uint64_t project_id)
{
	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open relations");
	const char *sql =
		"SELECT s.file_path, s.kind, s.name, s.start_row, "
		"       t.file_path, t.kind, t.name, t.start_row, r.type "
		"FROM relation r "
		"JOIN entity s ON s.id = r.source_id "
		"JOIN entity t ON t.id = r.target_id "
		"WHERE r.project_id=? "
		"ORDER BY s.file_path, s.kind, s.name, s.start_row, "
		"         t.file_path, t.kind, t.name, t.start_row, r.type";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare dumpRelations");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::vector<std::string> out;
	while (sqlite3_step(st) == SQLITE_ROW) {
		std::string line = col(st, 0) + "|" +
				   std::to_string(sqlite3_column_int(st, 1)) +
				   "|" + col(st, 2) + "|" +
				   std::to_string(sqlite3_column_int(st, 3));
		line += " -> ";
		line += col(st, 4) + "|" +
			std::to_string(sqlite3_column_int(st, 5)) + "|" +
			col(st, 6) + "|" +
			std::to_string(sqlite3_column_int(st, 7));
		line += " #" + std::to_string(sqlite3_column_int(st, 8));
		out.push_back(line);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	return out;
}

/// Report the first difference between two dumps (or an empty string).
static std::string firstDiff(const std::vector<std::string> &a,
			     const std::vector<std::string> &b)
{
	if (a.size() != b.size())
		return "size " + std::to_string(a.size()) + " vs " +
		       std::to_string(b.size());
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i] != b[i])
			return "[" + std::to_string(i) + "] " + a[i] +
			       "  !=  " + b[i];
	return "";
}

// ── Controlled-order section ─────────────────────────────────────
//
// The end-to-end comparison above depends on the parse workers actually
// settling the files in different orders, which is timing dependent. This
// section removes that dependency: it drives GraphStore directly and
// inserts the same two files' records in BOTH orders, then asserts the
// resulting ids are identical and follow the semantic key (file_path)
// rather than the insertion order. That is the deterministic core of the
// guarantee.

/// The single file used by the controlled-order section.
static const char *kOrderFile = "/order/only.cpp";

/// The two Function records inserted by the controlled-order section. Their
/// declaration positions are FIXED (Alpha at row 1, Axe at row 5), so the
/// record set is identical between the two runs and only the insert order
/// differs.
static std::vector<ir::Record> fixedOrderRecords()
{
	std::vector<ir::Record> recs;
	auto make = [](const char *name, uint32_t row, uint64_t id) {
		ir::Record r;
		r.id = id;
		r.original_id = id;
		r.kind = ir::RecordKind::Function;
		r.name = name;
		r.qualified_name = name;
		r.file_path = kOrderFile;
		r.language = "cpp";
		r.loc = ir::SourceRange{ row, 0, row + 1, 0 };
		return r;
	};
	recs.push_back(make("Alpha", 1, 1));
	recs.push_back(make("Axe", 5, 2));
	return recs;
}

/// Build a map of entity name -> entity id for the project.
static std::map<std::string, uint64_t> entityIds(store::GraphStore &store,
						 uint64_t project_id)
{
	sqlite3 *db = store.handle();
	const char *sql = "SELECT name, id FROM entity WHERE project_id=? "
			  "ORDER BY name";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare entityIds");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::map<std::string, uint64_t> ids;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		ids[n ? n : ""] =
			static_cast<uint64_t>(sqlite3_column_int64(st, 1));
	}
	sqlite3_finalize(st);
	return ids;
}

/// Insert the same two records forward or reversed and return the
/// resulting name -> id map.
///
/// One file only: with several files SQLite can pick an index ordered by
/// file_path, which hides an unordered ROW_NUMBER. Within one file_path the
/// index ties and the scan falls back to rowid, i.e. the insert order —
/// exactly the dependency this test must expose.
static std::map<std::string, uint64_t> runControlledOrder(bool reversed)
{
	const char *db_path = reversed ?
				      "/tmp/test_index_determinism_ord_rev.db" :
				      "/tmp/test_index_determinism_ord_fwd.db";
	unlink(db_path);

	store::GraphStore store;
	check(store.open(db_path), "open controlled-order database");
	uint64_t pid = store.createProject("/order", "order");
	check(pid > 0, "createProject (controlled order)");

	std::vector<ir::Record> recs = fixedOrderRecords();
	if (reversed)
		std::swap(recs[0], recs[1]);
	store.insertSemanticRecords(pid, kOrderFile, recs);

	check(store.buildGraph(pid, false), "buildGraph (controlled order)");

	std::map<std::string, uint64_t> ids = entityIds(store, pid);
	store.close();
	unlink(db_path);
	return ids;
}

int main()
{
	const std::string root = "/tmp/codescope_determinism";
	buildFixture(root);

	const char *db1 = "/tmp/test_index_determinism_1.db";
	const char *db2 = "/tmp/test_index_determinism_2.db";
	// Different worker counts: same input, different settling order.
	uint64_t pid1 = indexInto(root, db1, 1);
	uint64_t pid2 = indexInto(root, db2, 8);

	std::vector<std::string> ent1 = dumpEntities(db1, pid1);
	std::vector<std::string> ent2 = dumpEntities(db2, pid2);
	std::vector<std::string> rel1 = dumpRelations(db1, pid1);
	std::vector<std::string> rel2 = dumpRelations(db2, pid2);

	printf("  [INFO] run 1: %zu entities, %zu relations\n", ent1.size(),
	       rel1.size());
	printf("  [INFO] run 2: %zu entities, %zu relations\n", ent2.size(),
	       rel2.size());

	// Guard against a vacuous pass on an empty/failed index.
	check(static_cast<long long>(ent1.size()) >= kMinEntities,
	      "fixture produced too few entities — index did not run");
	check(static_cast<long long>(rel1.size()) >= kMinRelations,
	      "fixture produced too few relations — resolution did not run");

	// Entity ids must be a pure function of the input (this is the part
	// ROW_NUMBER() OVER () broke).
	std::string ent_diff = firstDiff(ent1, ent2);
	if (!ent_diff.empty())
		fprintf(stderr, "FAIL: entity dump differs: %s\n",
			ent_diff.c_str());
	check(ent_diff.empty(),
	      "entity ids are not reproducible: indexing the same input "
	      "twice produced different entities");
	printf("  [PASS] entity ids reproducible across two index runs\n");

	std::string rel_diff = firstDiff(rel1, rel2);
	if (!rel_diff.empty())
		fprintf(stderr, "FAIL: relation dump differs: %s\n",
			rel_diff.c_str());
	check(rel_diff.empty(),
	      "call graph is not reproducible: indexing the same input "
	      "twice produced different relations");
	printf("  [PASS] call graph reproducible across two index runs\n");

	// ── Controlled insertion order (deterministic, timing independent) ──
	std::map<std::string, uint64_t> ids_fwd = runControlledOrder(false);
	std::map<std::string, uint64_t> ids_rev = runControlledOrder(true);

	check(ids_fwd.size() == 2,
	      "controlled-order fixture lost entities (expected 2)");
	check(ids_fwd == ids_rev,
	      "entity ids depend on the semantic_records insertion order: "
	      "inserting the same two records in reverse order produced "
	      "different ids");
	// Alpha is declared at row 1 and Axe at row 5, so the semantic key
	// (start_row) must give Alpha the lower id.
	check(ids_fwd.at("Alpha") < ids_fwd.at("Axe"),
	      "entity ids do not follow the semantic key (start_row): "
	      "Alpha is declared first in the file and must own the lower id");
	printf("  [PASS] ids follow the semantic key, not the insertion "
	       "order\n");

	unlink(db1);
	unlink(db2);
	std::filesystem::remove_all(root);
	printf("\nAll index determinism tests passed.\n");
	return 0;
}
