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
				     std::string, int, int, int, int> > &rows)
{
	if (rows.empty())
		return true;

	beginTransaction();

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
			commitTransaction();
			return false;
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

		int rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"[module=store, method=insertTypeInfoBatch] "
				"step failed (rc=%d): %s\n",
				rc, sqlite3_errmsg(db_));
		}
		sqlite3_finalize(stmt);
	}

	commitTransaction();
	return true;
}

// ─── Type Ref Batch Insert ───────────────────────────────────────

bool GraphStore::insertTypeRefBatch(
	uint64_t project_id,
	const std::vector<std::tuple<uint64_t, std::string, int, std::string,
				     int, int> > &rows)
{
	if (rows.empty())
		return true;

	beginTransaction();

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
			commitTransaction();
			return false;
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
		if (rc != SQLITE_DONE) {
			fprintf(stderr,
				"[module=store, method=insertTypeRefBatch] "
				"step failed (rc=%d): %s\n",
				rc, sqlite3_errmsg(db_));
		}
		sqlite3_finalize(stmt);
	}

	commitTransaction();
	return true;
}

} // namespace store