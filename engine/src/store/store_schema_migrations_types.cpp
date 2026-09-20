// store_schema_migrations_types.cpp — route / type_info / type_ref migration.
//
// Split out of store_schema_migrations.cpp (plan/rules/code_rules.md: no source
// file may exceed 1000 lines). It runs between the other migration groups in
// GraphStore::runSchemaMigrations, which hands its failure-recording callback in
// as `record`.
//
// Every statement runs through `run`, which delegates to `record` so a failure
// is recorded together with the failing SQL (see GraphStore::runSchemaMigrations).
// Eight DDL calls in this block called exec() directly and ignored the result, so
// a database could end up without the type_ref indexes while the migration pass
// still reported success.

#include "store.h"

#include <sqlite3.h>

#include <functional>
#include <string>

namespace store
{

bool GraphStore::migrateTypeTables(
	const std::function<bool(const char *)> &record)
{
	// `run` routes the statements that used to call exec() directly through
	// `record`, so their failures are reported like every other one, and keeps
	// the local flag so the caller learns whether the whole group completed.
	bool ok = true;
	auto run = [&](const char *sql) -> bool {
		const bool done = record(sql);
		ok = done && ok;
		return done;
	};

	// Migration: add type_info + type_ref tables (v0.6+)
	{
		// Add route table if missing
		sqlite3_stmt *rprobe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT name FROM sqlite_master "
				       "WHERE type='table' AND name='route'",
				       -1, &rprobe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(rprobe) != SQLITE_ROW) {
				sqlite3_finalize(rprobe);
				run("CREATE TABLE IF NOT EXISTS route ("
				    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
				    " project_id INTEGER NOT NULL,"
				    " method TEXT NOT NULL,"
				    " path TEXT NOT NULL,"
				    " handler_name TEXT DEFAULT '',"
				    " file_path TEXT NOT NULL,"
				    " start_row INTEGER DEFAULT 0,"
				    " start_col INTEGER DEFAULT 0,"
				    " FOREIGN KEY (project_id) REFERENCES projects(id)"
				    ")");
				run("CREATE INDEX IF NOT EXISTS idx_route_path "
				    "ON route(project_id, method, path)");
			} else {
				sqlite3_finalize(rprobe);
			}
		}

		// Add type_name column to semantic_records if missing
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(semantic_records)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_type_name = false;
			bool has_call_kind = false;
			bool has_resolve_strategy = false;
			bool has_qualified_target = false;
			bool has_receiver_text = false;
			bool has_receiver_type = false;
			bool has_import_alias = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col) {
					const std::string c(col);
					if (c == "type_name")
						has_type_name = true;
					if (c == "call_kind")
						has_call_kind = true;
					if (c == "resolve_strategy")
						has_resolve_strategy = true;
					if (c == "qualified_target")
						has_qualified_target = true;
					if (c == "receiver_text")
						has_receiver_text = true;
					if (c == "receiver_type")
						has_receiver_type = true;
					if (c == "import_alias")
						has_import_alias = true;
				}
			}
			sqlite3_finalize(probe);
			// Null out the handle so a later accidental reuse is a
			// no-op (sqlite3_finalize(nullptr) is safe) instead of a
			// use-after-free. See the type_info block below.
			probe = nullptr;
			if (!has_type_name) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN type_name TEXT DEFAULT ''");
			}
			if (!has_call_kind) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN call_kind INTEGER DEFAULT 0");
			}
			if (!has_resolve_strategy) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN resolve_strategy "
				       "TEXT DEFAULT ''");
			}
			// Step 3 (plan §3.1): structured call-fact columns.
			if (!has_qualified_target) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN qualified_target "
				       "TEXT DEFAULT ''");
			}
			if (!has_receiver_text) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN receiver_text "
				       "TEXT DEFAULT ''");
			}
			if (!has_receiver_type) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN receiver_type "
				       "TEXT DEFAULT ''");
			}
			if (!has_import_alias) {
				record("ALTER TABLE semantic_records "
				       "ADD COLUMN import_alias "
				       "TEXT DEFAULT ''");
			}
		}

		// Step 3 (plan §3.1): migrate the `reference` table with the
		// same structured call-fact columns plus call_site_file. SQLite
		// has no ADD COLUMN IF NOT EXISTS, so probe table_info first.
		{
			sqlite3_stmt *ref_probe = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "PRAGMA table_info(reference)", -1,
				    &ref_probe, nullptr) == SQLITE_OK) {
				bool has_qualified_target = false;
				bool has_receiver_text = false;
				bool has_receiver_type = false;
				bool has_import_alias = false;
				bool has_call_site_file = false;
				while (sqlite3_step(ref_probe) == SQLITE_ROW) {
					const char *col =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								ref_probe, 1));
					if (col) {
						const std::string c(col);
						if (c == "qualified_target")
							has_qualified_target =
								true;
						if (c == "receiver_text")
							has_receiver_text =
								true;
						if (c == "receiver_type")
							has_receiver_type =
								true;
						if (c == "import_alias")
							has_import_alias = true;
						if (c == "call_site_file")
							has_call_site_file =
								true;
					}
				}
				sqlite3_finalize(ref_probe);
				if (!has_qualified_target)
					record("ALTER TABLE reference ADD COLUMN "
					       "qualified_target TEXT DEFAULT ''");
				if (!has_receiver_text)
					record("ALTER TABLE reference ADD COLUMN "
					       "receiver_text TEXT DEFAULT ''");
				if (!has_receiver_type)
					record("ALTER TABLE reference ADD COLUMN "
					       "receiver_type TEXT DEFAULT ''");
				if (!has_import_alias)
					record("ALTER TABLE reference ADD COLUMN "
					       "import_alias TEXT DEFAULT ''");
				if (!has_call_site_file)
					record("ALTER TABLE reference ADD COLUMN "
					       "call_site_file TEXT DEFAULT ''");
			}
		}

		// Step 6 (plan §6.1): migrate the `relation` table with
		// provenance columns. SQLite has no ADD COLUMN IF NOT EXISTS,
		// so probe table_info first. Each new column is nullable with
		// a default so pre-existing rows and non-call relations are
		// not affected.
		{
			sqlite3_stmt *probe = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "PRAGMA table_info(relation)", -1,
				    &probe, nullptr) == SQLITE_OK) {
				bool has_confidence = false;
				bool has_resolver = false;
				bool has_res_kind = false;
				bool has_reason = false;
				bool has_csf = false;
				bool has_csr = false;
				bool has_csc = false;
				while (sqlite3_step(probe) == SQLITE_ROW) {
					const char *col =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								probe, 1));
					if (!col)
						continue;
					std::string c = col;
					if (c == "confidence")
						has_confidence = true;
					else if (c == "resolver")
						has_resolver = true;
					else if (c == "resolution_kind")
						has_res_kind = true;
					else if (c == "reason")
						has_reason = true;
					else if (c == "call_site_file")
						has_csf = true;
					else if (c == "call_site_row")
						has_csr = true;
					else if (c == "call_site_col")
						has_csc = true;
				}
				sqlite3_finalize(probe);
				if (!has_confidence)
					record("ALTER TABLE relation ADD COLUMN "
					       "confidence REAL DEFAULT 0.0");
				if (!has_resolver)
					record("ALTER TABLE relation ADD COLUMN "
					       "resolver TEXT DEFAULT ''");
				if (!has_res_kind)
					record("ALTER TABLE relation ADD COLUMN "
					       "resolution_kind TEXT DEFAULT ''");
				if (!has_reason)
					record("ALTER TABLE relation ADD COLUMN "
					       "reason TEXT DEFAULT ''");
				if (!has_csf)
					record("ALTER TABLE relation ADD COLUMN "
					       "call_site_file TEXT DEFAULT ''");
				if (!has_csr)
					record("ALTER TABLE relation ADD COLUMN "
					       "call_site_row INTEGER DEFAULT 0");
				if (!has_csc)
					record("ALTER TABLE relation ADD COLUMN "
					       "call_site_col INTEGER DEFAULT 0");
			}
		}

		// Create type_info table if missing
		sqlite3_stmt *probe2 = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT name FROM sqlite_master "
				       "WHERE type='table' AND name='type_info'",
				       -1, &probe2, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe2) != SQLITE_ROW) {
				sqlite3_finalize(probe2);
				run("CREATE TABLE IF NOT EXISTS type_info ("
				    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
				    " project_id INTEGER NOT NULL,"
				    " name TEXT NOT NULL,"
				    " qualified_name TEXT DEFAULT '',"
				    " kind INTEGER NOT NULL,"
				    " file_path TEXT NOT NULL,"
				    " language TEXT DEFAULT '',"
				    " start_row INTEGER DEFAULT 0,"
				    " start_col INTEGER DEFAULT 0,"
				    " end_row INTEGER DEFAULT 0,"
				    " end_col INTEGER DEFAULT 0,"
				    " FOREIGN KEY (project_id) REFERENCES projects(id)"
				    ")");
				run("CREATE INDEX IF NOT EXISTS idx_ti_name "
				    "ON type_info(project_id, name)");
				run("CREATE INDEX IF NOT EXISTS idx_ti_qn "
				    "ON type_info(project_id, qualified_name)");
			} else {
				// Finalize probe2, NOT the outer `probe` from the
				// semantic_records migration above: that statement
				// was already finalized and would be a
				// use-after-free here (and probe2 would leak).
				sqlite3_finalize(probe2);
			}
		}
	}
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "SELECT name FROM sqlite_master "
				       "WHERE type='table' AND name='type_ref'",
				       -1, &probe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) != SQLITE_ROW) {
				sqlite3_finalize(probe);
				run("CREATE TABLE IF NOT EXISTS type_ref ("
				    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
				    " project_id INTEGER NOT NULL,"
				    " entity_id INTEGER NOT NULL,"
				    " type_name TEXT NOT NULL,"
				    " kind INTEGER NOT NULL,"
				    " file_path TEXT NOT NULL,"
				    " start_row INTEGER DEFAULT 0,"
				    " start_col INTEGER DEFAULT 0,"
				    " FOREIGN KEY (project_id) REFERENCES projects(id),"
				    " FOREIGN KEY (entity_id) REFERENCES entity(id)"
				    ")");
				run("CREATE INDEX IF NOT EXISTS idx_tr_type "
				    "ON type_ref(project_id, type_name)");
				run("CREATE INDEX IF NOT EXISTS idx_tr_entity "
				    "ON type_ref(project_id, entity_id)");
			} else {
				sqlite3_finalize(probe);
			}
		}
	}

	return ok;
}

} // namespace store
