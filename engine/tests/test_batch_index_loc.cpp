// test_batch_index_loc.cpp — the batch index path records what it measured.
//
// `insertFileResultBatch` is the writer a full index actually uses (the
// scheduler's chunk workers never call upsertFile). It used to insert a files
// stub with content_hash='' and no LOC at all:
//
//   * worktree_digest hashes (path, files.content_hash), so an all-empty column
//     made every source state look identical — compare_snapshots could never
//     report source_changed;
//   * files.loc stayed NULL, so the briefing reported "loc: unknown" and
//     file_size_limit ran with no input while claiming to have executed.
//
// Both are pinned here, plus the rule that a re-index keeps the row id (the
// evidence_fact reference) and refreshes the measurement.

#include "../src/ir/semantic_unit.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

static char kDbPath[160];
static char kFilePath[200];

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void removeFile(const char *path)
{
	std::remove(path);
	std::string wal = std::string(path) + "-wal";
	std::string shm = std::string(path) + "-shm";
	std::remove(wal.c_str());
	std::remove(shm.c_str());
}

static void writeSource(const std::string &content)
{
	std::ofstream out(kFilePath, std::ios::binary | std::ios::trunc);
	out << content;
}

static int64_t scalar(store::GraphStore &store, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st,
				 nullptr) == SQLITE_OK,
	      "prepare scalar");
	int64_t value = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return value;
}

static std::string textAt(store::GraphStore &store, const std::string &sql)
{
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(store.handle(), sql.c_str(), -1, &st,
				 nullptr) == SQLITE_OK,
	      "prepare textAt");
	std::string value;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *t = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (t)
			value = t;
	}
	sqlite3_finalize(st);
	return value;
}

/// Index one file through the batch path (the FFI's own writer).
static void indexThroughBatch(store::GraphStore &store, uint64_t project_id,
			      const std::string &path, int64_t mtime)
{
	std::vector<store::FileResult> batch;
	store::FileResult fr;
	fr.file_path = path;
	fr.language = "cpp";
	fr.mtime = mtime;
	fr.fsize = 0;
	batch.push_back(fr);
	check(store.insertFileResultBatch(project_id, batch),
	      "insertFileResultBatch must succeed");
}

int main()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_batch_loc_%d.db", int(getpid()));
	std::snprintf(kFilePath, sizeof(kFilePath),
		      "/tmp/codescope_batch_loc_src_%d.cpp", int(getpid()));
	removeFile(kDbPath);

	store::GraphStore store;
	check(store.open(kDbPath), "open database");
	const uint64_t project_id =
		store.createProject("/tmp/batch-loc", "batch");
	check(project_id > 0, "create project");

	// 3 code lines, 1 blank, 1 comment: loc=5, code=3, comment=1, blank=1.
	writeSource("int a;\n\n// note\nint b;\nint c;\n");
	indexThroughBatch(store, project_id, kFilePath, 1000);

	const std::string id_sql = "SELECT id FROM files WHERE project_id=" +
				   std::to_string(project_id);
	const int64_t first_id = scalar(store, id_sql);
	check(first_id > 0, "the batch path must create the files row");
	check(scalar(store, "SELECT loc FROM files WHERE id=" +
				    std::to_string(first_id)) == 5,
	      "loc must be measured on the batch path");
	check(scalar(store, "SELECT code_loc FROM files WHERE id=" +
				    std::to_string(first_id)) == 3,
	      "code_loc must partition the file");
	check(scalar(store, "SELECT comment_loc FROM files WHERE id=" +
				    std::to_string(first_id)) == 1,
	      "comment_loc must partition the file");
	check(scalar(store, "SELECT blank_loc FROM files WHERE id=" +
				    std::to_string(first_id)) == 1,
	      "blank_loc must partition the file");
	check(!textAt(store, "SELECT content_hash FROM files WHERE id=" +
				     std::to_string(first_id))
		       .empty(),
	      "content_hash must be recorded, or the digest is content-blind");

	const std::string digest_before = store.worktreeDigest(project_id);
	check(!digest_before.empty(), "worktree digest");

	// Same path, different content: the digest must move, and the row id must
	// survive so evidence_fact (4, files.id) keeps pointing at this file.
	writeSource("int a;\nint b;\n"); // 2 lines
	indexThroughBatch(store, project_id, kFilePath, 2000);
	check(scalar(store, id_sql) == first_id,
	      "re-indexing must keep files.id (evidence_fact references it)");
	check(scalar(store, "SELECT loc FROM files WHERE id=" +
				    std::to_string(first_id)) == 2,
	      "re-indexing must refresh the measurement");
	check(store.worktreeDigest(project_id) != digest_before,
	      "a changed file must change the worktree digest");

	// An unreadable path must leave the row unmeasured (NULL semantics would
	// need the column to be nullable; the batch path writes 0 and no hash).
	std::remove(kFilePath);
	const std::string digest_after_delete =
		store.worktreeDigest(project_id);
	check(!digest_after_delete.empty(),
	      "a missing source file must not break the digest");

	store.close();
	removeFile(kDbPath);
	removeFile(kFilePath);
	std::printf("test_batch_index_loc: all tests passed\n");
	return 0;
}
