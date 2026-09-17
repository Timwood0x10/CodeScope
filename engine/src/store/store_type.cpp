// store_type.cpp — Type registry batch insert implementation.
//
// Provides batch insert methods for type_info and type_ref tables,
// following the same multi-VALUES pattern as store_batch.cpp.
// Extracted into its own file to keep each translation unit under
// the 1000-line limit imposed by plan/rules/code_rules.md.
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/extract_type_refs.c — type reference extraction pattern
//   internal/cbm/extract_type_assigns.c — type assignment tracking pattern

#include "store.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace store
{

// ─── Constants ────────────────────────────────────────────────────

/** Max rows per multi-VALUES batch (keeps SQL string under ~64 KB). */
static constexpr int kTypeBatchSize = 500;

// ─── Type Info Batch Insert ───────────────────────────────────────

bool GraphStore::insertTypeInfoBatch(
	uint64_t project_id,
	const std::vector<std::tuple<std::string, std::string, int, std::string,
				     std::string, int, int, int, int>> &rows)
{
	if (rows.empty())
		return true;

	// Nested SAVEPOINT instead of BEGIN/COMMIT: this runs inside the index
	// transaction, where a plain BEGIN fails and the matching COMMIT would
	// commit the caller's transaction (and destroy any enclosing savepoint).
	if (!exec("SAVEPOINT insert_type_info")) {
		fprintf(stderr,
			"[module=store, method=insertTypeInfoBatch] "
			"SAVEPOINT failed: %s\n",
			error_.c_str());
		return false;
	}

	bool ok = true;
	for (size_t off = 0; off < rows.size(); off += kTypeBatchSize) {
		size_t batch = rows.size() - off;
		if (batch > static_cast<size_t>(kTypeBatchSize))
			batch = static_cast<size_t>(kTypeBatchSize);

		std::string sql =
			"INSERT INTO type_info "
			"(project_id, name, qualified_name, kind, "
			" file_path, language, "
			" start_row, start_col, end_row, end_col) VALUES ";

		for (size_t i = 0; i < batch; i++) {
			if (i > 0)
				sql += ",";
			char buf[32];
			snprintf(buf, sizeof(buf), "%llu",
				 (unsigned long long)project_id);
			sql += "(" + std::string(buf) + ",?,?,?,?,?,?,?,?)";
		}

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=insertTypeInfoBatch] "
				"prepare failed: %s\n",
				sqlite3_errmsg(db_));
			ok = false;
			break;
		}

		int idx = 1;
		for (size_t i = 0; i < batch; i++) {
			const auto &row = rows[off + i];
			sqlite3_bind_text(stmt, idx++, std::get<0>(row).c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, idx++, std::get<1>(row).c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, idx++, std::get<2>(row));
			sqlite3_bind_text(stmt, idx++, std::get<3>(row).c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, idx++, std::get<4>(row).c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, idx++, std::get<5>(row));
			sqlite3_bind_int(stmt, idx++, std::get<6>(row));
			sqlite3_bind_int(stmt, idx++, std::get<7>(row));
			sqlite3_bind_int(stmt, idx++, std::get<8>(row));
		}

		// A failed batch must not be reported as success: the caller
		// relies on the return value to decide whether to commit.
		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"[module=store, method=insertTypeInfoBatch] "
				"step failed (rc=%d): %s\n",
				rc, sqlite3_errmsg(db_));
			ok = false;
			break;
		}
	}

	if (!ok) {
		exec("ROLLBACK TO SAVEPOINT insert_type_info");
		exec("RELEASE SAVEPOINT insert_type_info");
		return false;
	}
	if (!exec("RELEASE SAVEPOINT insert_type_info")) {
		fprintf(stderr,
			"[module=store, method=insertTypeInfoBatch] "
			"RELEASE SAVEPOINT failed: %s\n",
			error_.c_str());
		exec("ROLLBACK TO SAVEPOINT insert_type_info");
		return false;
	}
	return true;
}

// ─── Type Ref Batch Insert ───────────────────────────────────────

bool GraphStore::insertTypeRefBatch(
	uint64_t project_id,
	const std::vector<std::tuple<uint64_t, std::string, int, std::string,
				     int, int>> &rows)
{
	if (rows.empty())
		return true;

	// Nested SAVEPOINT — see insertTypeInfoBatch above for why a plain
	// BEGIN/COMMIT is wrong here.
	if (!exec("SAVEPOINT insert_type_ref")) {
		fprintf(stderr,
			"[module=store, method=insertTypeRefBatch] "
			"SAVEPOINT failed: %s\n",
			error_.c_str());
		return false;
	}

	bool ok = true;
	for (size_t off = 0; off < rows.size(); off += kTypeBatchSize) {
		size_t batch = rows.size() - off;
		if (batch > static_cast<size_t>(kTypeBatchSize))
			batch = static_cast<size_t>(kTypeBatchSize);

		std::string sql = "INSERT INTO type_ref "
				  "(project_id, entity_id, type_name, kind, "
				  " file_path, start_row, start_col) VALUES ";

		for (size_t i = 0; i < batch; i++) {
			if (i > 0)
				sql += ",";
			char buf[32];
			snprintf(buf, sizeof(buf), "%llu",
				 (unsigned long long)project_id);
			sql += "(" + std::string(buf) + ",?,?,?,?,?,?)";
		}

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=insertTypeRefBatch] "
				"prepare failed: %s\n",
				sqlite3_errmsg(db_));
			ok = false;
			break;
		}

		int idx = 1;
		for (size_t i = 0; i < batch; i++) {
			const auto &row = rows[off + i];
			sqlite3_bind_int64(
				stmt, idx++,
				static_cast<int64_t>(std::get<0>(row)));
			sqlite3_bind_text(stmt, idx++, std::get<1>(row).c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, idx++, std::get<2>(row));
			sqlite3_bind_text(stmt, idx++, std::get<3>(row).c_str(),
					  -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(stmt, idx++, std::get<4>(row));
			sqlite3_bind_int(stmt, idx++, std::get<5>(row));
		}

		int rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"[module=store, method=insertTypeRefBatch] "
				"step failed (rc=%d): %s\n",
				rc, sqlite3_errmsg(db_));
			ok = false;
			break;
		}
	}

	if (!ok) {
		exec("ROLLBACK TO SAVEPOINT insert_type_ref");
		exec("RELEASE SAVEPOINT insert_type_ref");
		return false;
	}
	if (!exec("RELEASE SAVEPOINT insert_type_ref")) {
		fprintf(stderr,
			"[module=store, method=insertTypeRefBatch] "
			"RELEASE SAVEPOINT failed: %s\n",
			error_.c_str());
		exec("ROLLBACK TO SAVEPOINT insert_type_ref");
		return false;
	}
	return true;
}

} // namespace store