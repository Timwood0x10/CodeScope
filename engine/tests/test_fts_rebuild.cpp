// test_fts_rebuild.cpp — regression test for stale/ghost FTS rows after a
// file is re-indexed (2026-09-27 review, FTS non-idempotency).
//
// Before the fix:
//   buildFTSFromGraph rebuilt code_fts / name_trgm with
//   `INSERT OR IGNORE INTO … (rowid, …) SELECT e.id, …` and nothing ever
//   deleted FTS rows. Re-indexing a file re-uses entity ids, so the OR IGNORE
//   collided with the stale rowid: the old symbol stayed in code_fts (a ghost
//   that searchUnifiedJson returns — its FTS branch does not JOIN entity) and
//   the new symbol was silently dropped. `search` then returned deleted names
//   and missed renamed ones.
//
// After the fix:
//   buildFTSFromGraph deletes the project's FTS rows first, so a rebuild is
//   idempotent: the renamed symbol is indexed and the old one is gone.
//
// This is a store-level test (no async knowledge-builder thread) so it is
// deterministic: it drives buildGraph + buildFTSFromGraph directly and reads
// code_fts back.

#include "../include/engine.h"
#include "../src/ir/semantic_unit.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
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

// Count code_fts rows for a given name in a project.
static int ftsCount(store::GraphStore &store, uint64_t pid, const char *name)
{
	sqlite3 *db = store.handle();
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(
		      db,
		      "SELECT COUNT(*) FROM code_fts WHERE project_id=? AND name=?",
		      -1, &st, nullptr) == SQLITE_OK,
	      "prepare ftsCount");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(pid));
	sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
	int n = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

static ir::Record fn(const char *name, const char *file, uint64_t id)
{
	ir::Record r;
	r.id = id;
	r.original_id = id;
	r.kind = ir::RecordKind::Function;
	r.name = name;
	r.qualified_name = name;
	r.file_path = file;
	r.language = "cpp";
	r.loc = ir::SourceRange{ 1, 0, 2, 0 };
	return r;
}

int main()
{
	const char *db_path = "/tmp/test_fts_rebuild.db";
	unlink(db_path);

	store::GraphStore store;
	check(store.open(db_path), "open database");
	uint64_t pid = store.createProject("/proj", "fts-rebuild");
	check(pid > 0, "createProject");

	const char *file = "/proj/pkg/a.cpp";

	// Pass 1: index a file defining alphaFunc.
	std::vector<ir::Record> r1{ fn("alphaFunc", file, 1) };
	store.insertSemanticRecords(pid, file, r1);
	check(store.buildGraph(pid, false), "buildGraph pass 1");
	store.buildFTSFromGraph(pid);
	check(store.error().empty(), "buildFTSFromGraph pass 1 ok");
	check(ftsCount(store, pid, "alphaFunc") == 1,
	      "alphaFunc must be in code_fts after first index");

	// Pass 2: re-index the same file, now defining betaFunc. This mirrors
	// the incremental re-index path (insertFileResultBatch with
	// is_reindex=true): delete the file's semantic_records and graph rows,
	// insert the new record, rebuild the graph and FTS.
	{
		sqlite3_stmt *del = nullptr;
		check(sqlite3_prepare_v2(
			      store.handle(),
			      "DELETE FROM semantic_records WHERE project_id=? "
			      "AND file_path=?",
			      -1, &del, nullptr) == SQLITE_OK,
		      "prepare delete semantic_records");
		sqlite3_bind_int64(del, 1, static_cast<int64_t>(pid));
		sqlite3_bind_text(del, 2, file, -1, SQLITE_TRANSIENT);
		check(sqlite3_step(del) == SQLITE_DONE,
		      "delete semantic_records");
		sqlite3_finalize(del);
	}
	check(store.deleteGraphDataByFile(pid, file), "delete old file data");
	std::vector<ir::Record> r2{ fn("betaFunc", file, 1) };
	store.insertSemanticRecords(pid, file, r2);
	check(store.buildGraph(pid, false), "buildGraph pass 2");
	store.buildFTSFromGraph(pid);
	check(store.error().empty(), "buildFTSFromGraph pass 2 ok");

	// The renamed symbol must be searchable (dropped before the fix).
	check(ftsCount(store, pid, "betaFunc") == 1,
	      "betaFunc must be in code_fts after re-index");
	// The old symbol must be gone (a ghost before the fix).
	check(ftsCount(store, pid, "alphaFunc") == 0,
	      "alphaFunc must NOT remain in code_fts after rename");

	store.close();
	unlink(db_path);
	printf("\n=== FTS rebuild idempotency test passed ===\n");
	return 0;
}
