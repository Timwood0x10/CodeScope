#include "contract.h"
#include <cstdio>
#include <sqlite3.h>

namespace model
{

ContractPlugin::ContractPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult ContractPlugin::build(uint64_t project_id)
{
	ModelResult r;
	r.plugin_name = "Contract";

	// Extract contracts from document table (type=0 = README).
	// Look for contract keywords: "thread safe", "memory safe", etc.
	const char *sql = "SELECT d.file_path, d.content FROM document d "
			  "WHERE d.project_id = ? AND d.type = 0 "
			  "AND d.content != '' LIMIT 5";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		r.error = "prepare document query failed";
		return r;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	int64_t contracts = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *content = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (!fp || !content)
			continue;

		std::string text(content);
		// Look for contract keywords
		const char *keywords[] = { "thread safe", "thread-safe",
					   "ThreadSafe",  "memory safe",
					   "memory-safe", "MemorySafe",
					   "zero-copy",	  "zero copy",
					   "lock-free",	  "lock free",
					   "not safe",	  "unsafe" };
		for (auto *kw : keywords) {
			if (text.find(kw) != std::string::npos) {
				store_->insertContract(project_id, kw, "readme",
						       kw, std::string(fp), 0);
				contracts++;
			}
		}
	}
	sqlite3_finalize(stmt);

	// Also scan TODO/FIXME comments for missing contracts
	const char *todo_sql =
		"SELECT e.file_path, e.name FROM entity e "
		"WHERE e.project_id = ? AND e.kind IN (0,1) "
		"AND e.name LIKE '%TODO%' OR e.name LIKE '%FIXME%' "
		"OR e.name LIKE '%HACK%' OR e.name LIKE '%XXX%' "
		"LIMIT 20";
	sqlite3_stmt *todo_st = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), todo_sql, -1, &todo_st,
			       nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(todo_st, 1,
				   static_cast<int64_t>(project_id));
		while (sqlite3_step(todo_st) == SQLITE_ROW) {
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(todo_st, 0));
			const char *name = reinterpret_cast<const char *>(
				sqlite3_column_text(todo_st, 1));
			if (!fp || !name)
				continue;
			store_->insertContract(project_id,
					       std::string("TODO: ") + name,
					       "comment", name, std::string(fp),
					       0);
			contracts++;
		}
		sqlite3_finalize(todo_st);
	}

	r.items_created = contracts;
	return r;
}

} // namespace model