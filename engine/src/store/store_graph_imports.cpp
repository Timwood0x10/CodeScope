// store_graph_imports.cpp — batch import writer used by buildGraph.
// See store_graph_imports.h for why this lives in its own TU.

#include "store_graph_imports.h"

#include <cstdio>

namespace store
{

bool flushImportBatch(sqlite3 *db, uint64_t project_id,
		      const std::vector<PathRec> &batch)
{
	if (batch.empty())
		return true;
	std::string sql = "INSERT OR IGNORE INTO import "
			  "(project_id, source_scope_id, target_path, alias, "
			  " file_path, is_pub) VALUES ";
	for (size_t i = 0; i < batch.size(); i++) {
		if (i > 0)
			sql += ",";
		sql += "(?,0,?,?,?,0)";
	}
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"[module=store, method=flushImportBatch] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return false;
	}
	for (size_t i = 0; i < batch.size(); i++) {
		int base = static_cast<int>(i * 4);
		sqlite3_bind_int64(stmt, base + 1,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_text(stmt, base + 2, batch[i].path.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, base + 3, batch[i].alias.c_str(), -1,
				  SQLITE_STATIC);
		sqlite3_bind_text(stmt, base + 4, batch[i].file.c_str(), -1,
				  SQLITE_STATIC);
	}
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
		fprintf(stderr,
			"[module=store, method=flushImportBatch] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return false;
	}
	sqlite3_finalize(stmt);
	return true;
}

} // namespace store
