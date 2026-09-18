// store_graph_csr.cpp — BLOB-packed call-edge adjacency (CSR).
//
// Split out of store_graph.cpp (see plan/rules/code_rules.md 1000-line
// rule). buildCSR packs relation(type=1) edges into per-node BLOBs in the
// adjacency / adjacency_rev tables so caller/callee lookups are O(1)
// instead of an O(n) scan of the relation table.
//
// The rebuild runs in a SAVEPOINT, so a mid-build failure rolls back
// atomically — a crash can never leave a half-populated CSR, which would
// silently drop query edges.

#include "store.h"

#include <cstdint>
#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace store
{

// ── CSR Adjacency (BLOB-packed call edges) ─────────────────────

bool GraphStore::buildCSR(uint64_t project_id)
{
	// Wrap the full rebuild (DELETE + forward inserts + reverse inserts)
	// in a SAVEPOINT so a mid-build failure rolls back atomically:
	// without this, a crash between the DELETE and the final INSERT leaves
	// a half-populated CSR (some edges silently missing from queries).
	// SAVEPOINT (not BEGIN) is required because buildGraph always runs
	// with an active transaction (SAVEPOINT buildGraph / caller BEGIN),
	// and SQLite forbids BEGIN inside an active transaction.
	if (!exec("SAVEPOINT buildCSR")) {
		fprintf(stderr,
			"[module=store, method=buildCSR] SAVEPOINT buildCSR "
			"failed: %s\n",
			sqlite3_errmsg(db_));
		return false;
	}

	// Clear previous entries for this project
	exec(std::string("DELETE FROM adjacency WHERE project_id=" +
			 std::to_string(project_id))
		     .c_str());

	// Read all call edges from relation table, ordered by source_id for streaming group-by.
	// ORDER BY ensures same caller rows are contiguous so we only flush
	// to the BLOB when the source changes.
	std::string sql = "SELECT source_id, target_id "
			  "FROM relation "
			  "WHERE type=1 AND project_id=" +
			  std::to_string(project_id) + " ORDER BY source_id";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) !=
	    SQLITE_OK) {
		exec("ROLLBACK TO SAVEPOINT buildCSR");
		exec("RELEASE SAVEPOINT buildCSR");
		return false;
	}

	// v0.6 (perf): rows for this project were just DELETEd above (line 767),
	// so no PK conflicts can exist — plain INSERT is equivalent to INSERT OR
	// REPLACE here but skips the replace path's conflict bookkeeping.
	const char *ins_sql =
		"INSERT INTO adjacency (src_id, project_id, tgt_blob) "
		"VALUES (?, ?, ?)";
	sqlite3_stmt *ins = nullptr;
	if (sqlite3_prepare_v2(db_, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
		sqlite3_finalize(st);
		exec("ROLLBACK TO SAVEPOINT buildCSR");
		exec("RELEASE SAVEPOINT buildCSR");
		return false;
	}

	int64_t pid_i = static_cast<int64_t>(project_id);
	int64_t current_src = -1;
	std::vector<uint64_t> buf;
	buf.reserve(1024);
	int64_t count = 0;

	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t src = sqlite3_column_int64(st, 0);
		int64_t tgt = sqlite3_column_int64(st, 1);
		if (src == tgt)
			continue; // skip self-loops

		if (src != current_src) {
			// Flush previous group
			if (current_src >= 0 && !buf.empty()) {
				sqlite3_bind_int64(ins, 1, current_src);
				sqlite3_bind_int64(ins, 2, pid_i);
				sqlite3_bind_blob(
					ins, 3, buf.data(),
					static_cast<int>(buf.size() *
							 sizeof(uint64_t)),
					SQLITE_STATIC);
				if (sqlite3_step(ins) == SQLITE_DONE)
					count++;
				else
					fprintf(stderr,
						"buildCSR: forward flush"
						" failed: %s\n",
						sqlite3_errmsg(db_));
				sqlite3_reset(ins);
			}
			current_src = src;
			buf.clear();
		}
		buf.push_back(static_cast<uint64_t>(tgt));
	}
	// Flush last group
	if (current_src >= 0 && !buf.empty()) {
		sqlite3_bind_int64(ins, 1, current_src);
		sqlite3_bind_int64(ins, 2, pid_i);
		sqlite3_bind_blob(
			ins, 3, buf.data(),
			static_cast<int>(buf.size() * sizeof(uint64_t)),
			SQLITE_STATIC);
		if (sqlite3_step(ins) == SQLITE_DONE)
			count++;
		else
			fprintf(stderr,
				"buildCSR: final forward flush failed: %s\n",
				sqlite3_errmsg(db_));
		sqlite3_reset(ins);
	}

	sqlite3_finalize(ins);
	sqlite3_finalize(st);
	fprintf(stderr, "buildCSR: %lld forward groups from relation(type=1)\n",
		(long long)count);

	// ── Build reverse adjacency (adjacency_rev) ──
	// Mirror of forward adjacency: group by target_id (callee) instead
	// of source_id (caller). Enables O(1) getCallerIds() lookups.
	exec(std::string("DELETE FROM adjacency_rev WHERE project_id=" +
			 std::to_string(project_id))
		     .c_str());

	std::string rev_sql = "SELECT target_id, source_id "
			      "FROM relation "
			      "WHERE type=1 AND project_id=" +
			      std::to_string(project_id) +
			      " ORDER BY target_id";
	sqlite3_stmt *rev_st = nullptr;
	if (sqlite3_prepare_v2(db_, rev_sql.c_str(), -1, &rev_st, nullptr) !=
	    SQLITE_OK) {
		exec("ROLLBACK TO SAVEPOINT buildCSR");
		exec("RELEASE SAVEPOINT buildCSR");
		return false;
	}

	// v0.6 (perf): rows for this project were just DELETEd above, so no PK
	// conflicts can exist — plain INSERT equals INSERT OR REPLACE here.
	const char *rev_ins_sql =
		"INSERT INTO adjacency_rev (tgt_id, project_id, "
		"src_blob) VALUES (?, ?, ?)";
	sqlite3_stmt *rev_ins = nullptr;
	if (sqlite3_prepare_v2(db_, rev_ins_sql, -1, &rev_ins, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(rev_st);
		exec("ROLLBACK TO SAVEPOINT buildCSR");
		exec("RELEASE SAVEPOINT buildCSR");
		return false;
	}

	int64_t current_tgt = -1;
	std::vector<uint64_t> rev_buf;
	rev_buf.reserve(1024);
	int64_t rev_count = 0;

	while (sqlite3_step(rev_st) == SQLITE_ROW) {
		int64_t tgt = sqlite3_column_int64(rev_st, 0);
		int64_t src = sqlite3_column_int64(rev_st, 1);
		if (src == tgt)
			continue;

		if (tgt != current_tgt) {
			if (current_tgt >= 0 && !rev_buf.empty()) {
				sqlite3_bind_int64(rev_ins, 1, current_tgt);
				sqlite3_bind_int64(rev_ins, 2, pid_i);
				sqlite3_bind_blob(
					rev_ins, 3, rev_buf.data(),
					static_cast<int>(rev_buf.size() *
							 sizeof(uint64_t)),
					SQLITE_STATIC);
				if (sqlite3_step(rev_ins) == SQLITE_DONE)
					rev_count++;
				else
					fprintf(stderr,
						"buildCSR: rev flush"
						" failed: %s\n",
						sqlite3_errmsg(db_));
				sqlite3_reset(rev_ins);
			}
			current_tgt = tgt;
			rev_buf.clear();
		}
		rev_buf.push_back(static_cast<uint64_t>(src));
	}
	if (current_tgt >= 0 && !rev_buf.empty()) {
		sqlite3_bind_int64(rev_ins, 1, current_tgt);
		sqlite3_bind_int64(rev_ins, 2, pid_i);
		sqlite3_bind_blob(
			rev_ins, 3, rev_buf.data(),
			static_cast<int>(rev_buf.size() * sizeof(uint64_t)),
			SQLITE_STATIC);
		if (sqlite3_step(rev_ins) == SQLITE_DONE)
			rev_count++;
		else
			fprintf(stderr,
				"buildCSR: final rev flush failed: %s\n",
				sqlite3_errmsg(db_));
		sqlite3_reset(rev_ins);
	}

	sqlite3_finalize(rev_ins);
	sqlite3_finalize(rev_st);
	fprintf(stderr, "buildCSR: %lld reverse groups from relation(type=1)\n",
		(long long)rev_count);
	// Release the atomic rebuild savepoint; a failure here leaves the
	// savepoint open, so roll back explicitly rather than leaking a
	// pending savepoint into the caller's transaction.
	if (!exec("RELEASE SAVEPOINT buildCSR")) {
		fprintf(stderr,
			"[module=store, method=buildCSR] RELEASE SAVEPOINT "
			"buildCSR failed: %s\n",
			sqlite3_errmsg(db_));
		exec("ROLLBACK TO SAVEPOINT buildCSR");
		exec("RELEASE SAVEPOINT buildCSR");
		return false;
	}
	return true;
}

/// Decode a packed uint64_t BLOB into a vector of node IDs.
///
/// The CSR adjacency tables store neighbor IDs as a packed array of
/// uint64_t. The BLOB length must be an exact multiple of sizeof(uint64_t);
/// a non-multiple indicates corruption or an externally-written row. The
/// trailing partial element is dropped and a diagnostic is emitted so the
/// caller is never silently handed a truncated neighbor list.
///
/// @param blob   Pointer to the BLOB bytes (may be null when length is 0).
/// @param bytes  Length of the BLOB in bytes.
/// @return The decoded neighbor IDs.
static std::vector<uint64_t> decodeAdjacencyBlob(const void *blob, int bytes)
{
	constexpr int kUint64Bytes = static_cast<int>(sizeof(uint64_t));
	if (bytes % kUint64Bytes != 0) {
		fprintf(stderr,
			"[module=store, method=decodeAdjacencyBlob] BLOB "
			"length %d is not a multiple of %d — trailing %d "
			"byte(s) dropped\n",
			bytes, kUint64Bytes, bytes % kUint64Bytes);
	}
	const int n = bytes / kUint64Bytes;
	std::vector<uint64_t> ids;
	ids.reserve(static_cast<size_t>(n));
	if (n > 0) {
		const auto *arr = static_cast<const uint64_t *>(blob);
		for (int i = 0; i < n; i++)
			ids.push_back(static_cast<uint64_t>(arr[i]));
	}
	return ids;
}

std::vector<uint64_t> GraphStore::getCalleeIds(uint64_t node_id)
{
	std::vector<uint64_t> ids;
	const char *sql = "SELECT tgt_blob FROM adjacency WHERE src_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
		return ids;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(node_id));
	if (sqlite3_step(st) == SQLITE_ROW) {
		ids = decodeAdjacencyBlob(sqlite3_column_blob(st, 0),
					  sqlite3_column_bytes(st, 0));
	}
	sqlite3_finalize(st);
	return ids;
}

std::vector<uint64_t> GraphStore::getCallerIds(uint64_t node_id)
{
	// O(1) reverse adjacency lookup via adjacency_rev table.
	// Falls back to O(n) full-scan if adjacency_rev is not populated
	// (e.g., buildCSR was called before the reverse adjacency feature).
	std::vector<uint64_t> ids;
	const char *sql = "SELECT src_blob FROM adjacency_rev WHERE tgt_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
		return ids;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(node_id));
	if (sqlite3_step(st) == SQLITE_ROW) {
		ids = decodeAdjacencyBlob(sqlite3_column_blob(st, 0),
					  sqlite3_column_bytes(st, 0));
		sqlite3_finalize(st);
		return ids;
	}
	sqlite3_finalize(st);

	// Fallback: O(n) full-scan of forward adjacency (legacy path)
	const char *fallback_sql =
		"SELECT src_id, tgt_blob FROM adjacency WHERE project_id IN "
		"(SELECT project_id FROM entity WHERE id=?)";
	if (sqlite3_prepare_v2(db_, fallback_sql, -1, &st, nullptr) !=
	    SQLITE_OK)
		return ids;
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(node_id));
	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t src = sqlite3_column_int64(st, 0);
		auto src_ids = decodeAdjacencyBlob(sqlite3_column_blob(st, 1),
						   sqlite3_column_bytes(st, 1));
		uint64_t target = node_id;
		for (uint64_t id : src_ids) {
			if (id == target) {
				ids.push_back(static_cast<uint64_t>(src));
				break;
			}
		}
	}
	sqlite3_finalize(st);
	return ids;
}
} // namespace store
