// store_schema_migrations.cpp — per-column schema migrations.
//
// Split out of store_schema.cpp (see plan/rules/code_rules.md 1000-line
// rule). createSchema() executes the main DDL and then calls this to patch
// databases that already exist: SQLite has no "ADD COLUMN IF NOT EXISTS",
// so every column added in a later version is probed with PRAGMA
// table_info and ALTER TABLE'd in here. CREATE TABLE IF NOT EXISTS does
// nothing for a table that already exists, so these migrations are the
// only reason an old database can be re-indexed without a full rebuild.
//
// A hard failure returns false so createSchema() fails loudly instead of
// leaving a half-migrated schema that later queries misinterpret.

#include "store.h"

#include <cstdio>
#include <sqlite3.h>
#include <string>

namespace store
{

bool GraphStore::runSchemaMigrations()
{
	// ── Schema migrations for pre-existing databases ───────────────
	// CREATE TABLE IF NOT EXISTS skips when the table already exists, so columns
	// added in later versions must be patched in here. SQLite has no
	// "ADD COLUMN IF NOT EXISTS", so we probe PRAGMA table_info first.

	// ── Migration failure collection ───────────────────────────────
	// Every ALTER TABLE below runs through migrationExec so a failure is
	// recorded and reported at the end of the pass. Thirty of these call sites
	// used to ignore the result while the function still returned true, leaving
	// a half-migrated schema whose queries later failed with "no such column" —
	// far from the cause (docs/CODE_REVIEW_2026-09-18.md #19). Wrapping the
	// calls in one place covers every existing site and any future one, and
	// catches any cause of a failed ALTER, not only a missing column.
	bool migration_ok = true;
	auto migrationExec = [&](const char *sql) -> bool {
		if (exec(sql))
			return true;
		migration_ok = false;
		fprintf(stderr,
			"[module=store, method=runSchemaMigrations] migration "
			"statement failed: %s | sql=%.100s\n",
			error().c_str(), sql ? sql : "(null)");
		return false;
	};

	// Migration: add knowledge_ready column to project_readiness (v0.5+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(project_readiness)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_knowledge_ready = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col &&
				    std::string(col) == "knowledge_ready")
					has_knowledge_ready = true;
			}
			sqlite3_finalize(probe);
			if (!has_knowledge_ready) {
				migrationExec(
					"ALTER TABLE project_readiness "
					"ADD COLUMN knowledge_ready INTEGER DEFAULT 0");
			}
		}
	}

	// Migration: add module_state column to entity table (v0.5+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_module_state = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "module_state")
					has_module_state = true;
			}
			sqlite3_finalize(probe);
			if (!has_module_state) {
				migrationExec("ALTER TABLE entity "
					      "ADD COLUMN module_state "
					      "INTEGER NOT NULL DEFAULT 0");
			}
		}
	}

	// Migration: add module_path column to entity table (v0.9+)
	// Denormalizes the directory portion of file_path so that scope
	// JOINs and module_edge grouping can use an indexed equality predicate
	// instead of the non-sargable rtrim(file_path, replace(...)) expression.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_module_path = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "module_path")
					has_module_path = true;
			}
			sqlite3_finalize(probe);
			if (!has_module_path) {
				// Wrap ALTER + backfill UPDATE + CREATE INDEX in
				// a single transaction. Without this, a crash after
				// ALTER leaves the column existing with '' values;
				// the next startup sees the column and skips the
				// migration, so the backfill never reruns and
				// scope/state_builder JOINs break silently.
				if (!exec("BEGIN IMMEDIATE")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"BEGIN module_path migration "
						"failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!migrationExec(
					    "ALTER TABLE entity "
					    "ADD COLUMN module_path "
					    "TEXT NOT NULL DEFAULT ''")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"ALTER TABLE entity ADD module_path "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
				// Backfill module_path for pre-existing entity rows
				// using the same rtrim(replace(...)) expression
				// used at INSERT time in store_graph.cpp. Without
				// this, migrated databases would have module_path=''
				// for all existing entities, breaking scope creation,
				// state_builder JOINs, and module_edge grouping.
				if (!exec("UPDATE entity SET module_path = "
					  "rtrim(file_path, "
					  "replace(file_path, '/', 'x')) "
					  "WHERE module_path = ''")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"UPDATE entity backfill module_path "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS "
					  "idx_entity_module "
					  "ON entity(project_id, module_path)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_entity_module "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
				if (!exec("COMMIT")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"COMMIT module_path migration "
						"failed: %s\n",
						error_.c_str());
					exec("ROLLBACK");
					return false;
				}
			}
		}
	}

	// Migration: add arity + is_static columns to semantic_records (v0.5+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(semantic_records)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_arity = false;
			bool has_is_static = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col) {
					if (std::string(col) == "arity")
						has_arity = true;
					if (std::string(col) == "is_static")
						has_is_static = true;
				}
			}
			sqlite3_finalize(probe);
			if (!has_arity) {
				migrationExec(
					"ALTER TABLE semantic_records "
					"ADD COLUMN arity INTEGER DEFAULT 0");
			}
			if (!has_is_static) {
				migrationExec(
					"ALTER TABLE semantic_records "
					"ADD COLUMN is_static INTEGER DEFAULT 0");
			}
		}
	}

	// Migration (v0.2.5): add code-metrics columns to entity for databases
	// created before the metrics restore. Each column defaults to 0 so
	// existing rows stay valid; resolveStagedMetrics fills them on the next
	// index/enhance run. Mirrors the entity DDL in createSchema().
	{
		struct EntityMetricCol {
			const char *name;
			const char *dflt;
		};
		static const EntityMetricCol kCols[] = {
			{ "cyclomatic", "0" }, { "nesting_depth", "0" },
			{ "cognitive", "0" },  { "param_count", "0" },
			{ "call_count", "0" }, { "branch_count", "0" },
			{ "loop_count", "0" }, { "lines", "0" },
			{ "is_stub", "0" },
		};
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			std::vector<std::string> existing;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col)
					existing.emplace_back(col);
			}
			sqlite3_finalize(probe);
			for (const auto &c : kCols) {
				if (std::find(existing.begin(), existing.end(),
					      c.name) != existing.end())
					continue;
				migrationExec(
					("ALTER TABLE entity ADD COLUMN " +
					 std::string(c.name) +
					 " INTEGER NOT NULL DEFAULT " + c.dflt)
						.c_str());
			}
		} else {
			fprintf(stderr,
				"createSchema: entity metrics migration probe "
				"failed: %s [module=store, method=createSchema]\n",
				sqlite3_errmsg(db_));
		}
	}

	// Migration (v0.2.5): add metrics_ready to project_readiness for
	// databases created before the metrics restore. Mirrors the DDL column
	// in createSchema().
	{
		sqlite3_stmt *rprobe = nullptr;
		bool has_metrics_ready = false;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(project_readiness)",
				       -1, &rprobe, nullptr) == SQLITE_OK) {
			while (sqlite3_step(rprobe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(rprobe, 1));
				if (col && std::string(col) == "metrics_ready")
					has_metrics_ready = true;
			}
			sqlite3_finalize(rprobe);
		}
		if (!has_metrics_ready) {
			migrationExec(
				"ALTER TABLE project_readiness "
				"ADD COLUMN metrics_ready INTEGER DEFAULT 0");
		}
	}

	// Route / type_info / type_ref live in their own TU (1000-line rule): see
	// store_schema_migrations_types.cpp. Failures inside it are recorded by
	// migrationExec; the return value is surfaced here so no path is silent.
	if (!migrateTypeTables(migrationExec))
		migration_ok = false;

	// Note: vec0 embeddings table is created in engine_init() after
	// sqlite-vec extension is loaded via dlopen. Not needed here.

	// Migration: add role column to module_summary table (v0.7+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(module_summary)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_role = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "role")
					has_role = true;
			}
			sqlite3_finalize(probe);
			if (!has_role) {
				migrationExec(
					"ALTER TABLE module_summary "
					"ADD COLUMN role TEXT DEFAULT ''");
			}
		}
	}

	// Migration: add visibility column to entity table (v0.2.2)
	// 0 = private (default), 1 = pub/public/export, 2 = protected (Java/C# reserved)
	// Populated by Visitors per language: Rust pub→1, Go exported-uppercase→1,
	// Python __leading→0 else→1, C/C++ header-declared→1 static-anon→0,
	// Java public→1 private→0 protected→2, JS/TS export→1 else→0, Swift public/open→1.
	// role classifier (state_builder.cpp buildModuleSummaries) fuses pub_count
	// from this column with call-graph counts — see docs/dev_plans/role_classifier_plan.md.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_visibility = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "visibility")
					has_visibility = true;
			}
			sqlite3_finalize(probe);
			if (!has_visibility) {
				migrationExec(
					"ALTER TABLE entity "
					"ADD COLUMN visibility INTEGER NOT NULL DEFAULT 0");
			}
		}
	}

	// Migration: add arity column to entity table (v0.5+, C2)
	// The Resolver Pipeline (resolver/pipeline.cpp) SELECTs entity.arity to
	// score same-name overload candidates via factorSignatureMatch. Without
	// this column the SELECT fails with "no such column: arity", breaking
	// the entire resolve run. Backfill from semantic_records (which already
	// has arity populated by Visitors) so pre-existing entity rows get the
	// correct arity without a re-index. Matches by (project_id, file_path,
	// name, start_row) — the same identity used by buildGraph's _r2n JOIN.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(entity)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_arity = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "arity")
					has_arity = true;
			}
			sqlite3_finalize(probe);
			if (!has_arity) {
				if (!migrationExec(
					    "ALTER TABLE entity "
					    "ADD COLUMN arity INTEGER NOT NULL DEFAULT 0")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"ALTER TABLE entity ADD arity "
						"failed: %s\n",
						error_.c_str());
					return false;
				}
				// Backfill arity from semantic_records. Each
				// declaration record (kind 0/1) carries the
				// visitor-computed argument count. Call records
				// (kind 9) are skipped — entity rows are
				// declarations only.
				if (!exec("UPDATE entity SET arity = COALESCE("
					  " (SELECT sr.arity FROM semantic_records sr"
					  "  WHERE sr.project_id = entity.project_id"
					  "  AND sr.file_path = entity.file_path"
					  "  AND sr.name = entity.name"
					  "  AND sr.start_row = entity.start_row"
					  "  AND sr.kind IN (0, 1)), 0)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"UPDATE entity backfill arity "
						"failed: %s\n",
						error_.c_str());
					return false;
				}
			}
		}
	}

	// Migration: add call_kind column to reference table (v0.7+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(reference)", -1,
				       &probe, nullptr) == SQLITE_OK) {
			bool has_ck = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "call_kind")
					has_ck = true;
			}
			sqlite3_finalize(probe);
			if (!has_ck) {
				migrationExec(
					"ALTER TABLE reference "
					"ADD COLUMN call_kind INTEGER DEFAULT 0");
			}
			// Check for resolve_strategy column (v0.9+)
			bool has_ref_rs = false;
			// Reuse the same probe - re-prepare
			probe = nullptr;
			if (sqlite3_prepare_v2(
				    db_, "PRAGMA table_info(reference)", -1,
				    &probe, nullptr) == SQLITE_OK) {
				while (sqlite3_step(probe) == SQLITE_ROW) {
					const char *col =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								probe, 1));
					if (col && std::string(col) ==
							   "resolve_strategy")
						has_ref_rs = true;
				}
				sqlite3_finalize(probe);
				probe = nullptr;
			}
			if (!has_ref_rs) {
				migrationExec("ALTER TABLE reference "
					      "ADD COLUMN resolve_strategy "
					      "TEXT DEFAULT ''");
			}
		}
	}

	// Migration: add parent_id column to graph_nodes (v0.8+)
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(graph_nodes)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_pid = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col && std::string(col) == "parent_id")
					has_pid = true;
			}
			sqlite3_finalize(probe);
			if (!has_pid) {
				migrationExec(
					"ALTER TABLE graph_nodes "
					"ADD COLUMN parent_id INTEGER DEFAULT 0");
				exec("CREATE INDEX IF NOT EXISTS idx_gn_parent "
				     "ON graph_nodes(project_id, parent_id)");
			}
		}
	}

	// Migration: add resolve_strategy column to graph_edges (v0.9+)
	// Propagated from semantic_records → reference → _resolved_edges
	// by the Resolver Pipeline. Stores the resolution strategy for
	// each call edge: "p1_intra" (intra-file resolved),
	// "external" (known builtin/third-party), "unresolved" (unknown).
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(db_, "PRAGMA table_info(graph_edges)",
				       -1, &probe, nullptr) == SQLITE_OK) {
			bool has_rs = false;
			while (sqlite3_step(probe) == SQLITE_ROW) {
				const char *col =
					reinterpret_cast<const char *>(
						sqlite3_column_text(probe, 1));
				if (col &&
				    std::string(col) == "resolve_strategy")
					has_rs = true;
			}
			sqlite3_finalize(probe);
			if (!has_rs) {
				migrationExec("ALTER TABLE graph_edges "
					      "ADD COLUMN resolve_strategy "
					      "TEXT DEFAULT ''");
			}
		}
	}

	// Migration: add semantic_fact table (v0.3 Phase 1).
	// The table is in the main schema string, but pre-existing
	// databases created before v0.3 need it added here. Probing
	// sqlite_master (not PRAGMA table_info) because the table may not
	// exist at all on legacy databases.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(
			    db_,
			    "SELECT name FROM sqlite_master "
			    "WHERE type='table' AND name='semantic_fact'",
			    -1, &probe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) != SQLITE_ROW) {
				sqlite3_finalize(probe);
				if (!exec("CREATE TABLE IF NOT EXISTS semantic_fact ("
					  " id            INTEGER PRIMARY KEY,"
					  " project_id    INTEGER NOT NULL,"
					  " function_id   INTEGER NOT NULL,"
					  " category      TEXT NOT NULL,"
					  " primitive     TEXT NOT NULL,"
					  " kind          TEXT NOT NULL,"
					  " symbol        TEXT NOT NULL DEFAULT '',"
					  " confidence    REAL NOT NULL DEFAULT 1.0,"
					  " detail_json   TEXT,"
					  " created_at    TEXT DEFAULT (datetime('now')),"
					  " FOREIGN KEY (project_id) REFERENCES projects(id),"
					  " FOREIGN KEY (function_id) REFERENCES graph_nodes(id)"
					  ")")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE TABLE semantic_fact failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_category "
					  "ON semantic_fact(project_id, category)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_category failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_primitive "
					  "ON semantic_fact(project_id, primitive)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_primitive failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_category_primitive "
					  "ON semantic_fact(project_id, category, primitive)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_category_primitive failed: %s\n",
						error_.c_str());
					return false;
				}
				if (!exec("CREATE INDEX IF NOT EXISTS idx_sf_function "
					  "ON semantic_fact(project_id, function_id)")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE INDEX idx_sf_function failed: %s\n",
						error_.c_str());
					return false;
				}
			} else {
				sqlite3_finalize(probe);
			}
		}
	}

	// Migration: add project_state table (v0.3 Phase 4 prep).
	// Schema is added now to avoid a future migration; the table is
	// unused until Phase 4. Same sqlite_master probe pattern as above.
	{
		sqlite3_stmt *probe = nullptr;
		if (sqlite3_prepare_v2(
			    db_,
			    "SELECT name FROM sqlite_master "
			    "WHERE type='table' AND name='project_state'",
			    -1, &probe, nullptr) == SQLITE_OK) {
			if (sqlite3_step(probe) != SQLITE_ROW) {
				sqlite3_finalize(probe);
				if (!exec("CREATE TABLE IF NOT EXISTS project_state ("
					  " id              INTEGER PRIMARY KEY,"
					  " project_id      INTEGER NOT NULL UNIQUE,"
					  " confidence      REAL NOT NULL DEFAULT 0.0,"
					  " snapshot_json   TEXT NOT NULL,"
					  " created_at      TEXT DEFAULT (datetime('now')),"
					  " updated_at      TEXT DEFAULT (datetime('now')),"
					  " FOREIGN KEY (project_id) REFERENCES projects(id)"
					  ")")) {
					fprintf(stderr,
						"[module=store, method=createSchema] "
						"CREATE TABLE project_state failed: %s\n",
						error_.c_str());
					return false;
				}
			} else {
				sqlite3_finalize(probe);
			}
		}
	}

	// Migration: collapse duplicate capability rows. The capability pass runs on
	// every index and the table has no uniqueness rule, so each run used to
	// append another copy of every capability; the insert is guarded now
	// (store_knowledge.cpp::insertCapability) and this removes the copies that
	// earlier runs left behind, so capability_state and the drift counts derived
	// from it stop double-counting.
	migrationExec(
		"DELETE FROM capability WHERE id NOT IN "
		"(SELECT MIN(id) FROM capability GROUP BY project_id, name, "
		" source_kind, source_ref)");

	// Migration: add cross_module_edges to architecture_state.
	//
	// architecture_state.violations used to hold the COUNT of cross-module
	// call pairs per module pair, with compliance forced to 0.0 — so every
	// normal dependency was reported as an architecture violation and
	// dragged the architecture score down. The count is real information;
	// the label was wrong. It now lives in its own column, `violations`
	// stays 0 until a real layer violation can be detected, and
	// compliance stays 1.0. Hard failures fail the schema build loudly
	// (see the header) instead of leaving a half-migrated table that
	// buildArchitectureState then fails to INSERT into.
	{
		sqlite3_stmt *arch_probe = nullptr;
		bool has_cross_module_edges = false;
		if (sqlite3_prepare_v2(db_,
				       "PRAGMA table_info(architecture_state)",
				       -1, &arch_probe, nullptr) == SQLITE_OK) {
			while (sqlite3_step(arch_probe) == SQLITE_ROW) {
				const char *col = reinterpret_cast<const char *>(
					sqlite3_column_text(arch_probe, 1));
				if (col &&
				    std::string(col) == "cross_module_edges")
					has_cross_module_edges = true;
			}
			sqlite3_finalize(arch_probe);
			arch_probe = nullptr;
		} else {
			fprintf(stderr,
				"[module=store, method=createSchema] "
				"architecture_state column probe "
				"failed: %s\n",
				error_.c_str());
			return false;
		}
		if (!has_cross_module_edges) {
			if (!migrationExec("ALTER TABLE architecture_state "
					   "ADD COLUMN cross_module_edges "
					   "INTEGER NOT NULL DEFAULT 0")) {
				fprintf(stderr,
					"[module=store, method=createSchema] "
					"ALTER TABLE architecture_state ADD "
					" cross_module_edges failed: %s\n",
					error_.c_str());
				return false;
			}
		}
		// Data correction. Both statements run on every open and
		// are idempotent, so a database that acquires bogus rows
		// later (a pre-fix binary after a downgrade) is repaired
		// on the next open instead of never.
		//
		// 1) Collapse duplicate (project_id, layer) rows. The old
		//    buildArchitectureState used INSERT OR IGNORE with no
		//    unique key to ignore on, so every rebuild appended a
		//    second copy of every row — and a re-index after the
		//    edge counts changed appended rows with different
		//    counts too. The NEWEST row is what the last rebuild
		//    wrote; summing the copies would inflate
		//    cross_module_edges by the number of rebuilds.
		if (!migrationExec("DELETE FROM architecture_state "
				   "WHERE rowid NOT IN "
				   "(SELECT MAX(rowid) FROM architecture_state "
				   " GROUP BY project_id, layer)")) {
			fprintf(stderr,
				"[module=store, method=createSchema] "
				"architecture_state de-duplication "
				"failed: %s\n",
				error_.c_str());
			return false;
		}
		// 2) Move the pre-fix `violations` value into the column
		//    that names it honestly rather than discarding it,
		//    and clear the bogus violation flag. Unconditional:
		//    gating it on the ALTER above would miss a table that
		//    already has the column but pre-fix rows. Afterwards
		//    violations is 0 and the WHERE matches nothing.
		if (!migrationExec("UPDATE architecture_state "
				   "SET cross_module_edges = violations, "
				   "    violations = 0, "
				   "    compliance = 1.0 "
				   "WHERE violations > 0")) {
			fprintf(stderr,
				"[module=store, method=createSchema] "
				"architecture_state violations move "
				"failed: %s\n",
				error_.c_str());
			return false;
		}
	}

	if (!migration_ok) {
		fprintf(stderr,
			"[module=store, method=runSchemaMigrations] one or more "
			"migrations failed; refusing to report a migrated schema "
			"(later queries would fail with \"no such column\")\n");
		return false;
	}

	return true;
}

} // namespace store
