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
				exec("ALTER TABLE project_readiness "
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
				exec("ALTER TABLE entity "
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
				if (!exec("ALTER TABLE entity "
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
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN arity INTEGER DEFAULT 0");
			}
			if (!has_is_static) {
				exec("ALTER TABLE semantic_records "
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
				exec(("ALTER TABLE entity ADD COLUMN " +
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
			exec("ALTER TABLE project_readiness "
			     "ADD COLUMN metrics_ready INTEGER DEFAULT 0");
		}
	}

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
				exec("CREATE TABLE IF NOT EXISTS route ("
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
				exec("CREATE INDEX IF NOT EXISTS idx_route_path "
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
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN type_name TEXT DEFAULT ''");
			}
			if (!has_call_kind) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN call_kind INTEGER DEFAULT 0");
			}
			if (!has_resolve_strategy) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN resolve_strategy "
				     "TEXT DEFAULT ''");
			}
			// Step 3 (plan §3.1): structured call-fact columns.
			if (!has_qualified_target) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN qualified_target "
				     "TEXT DEFAULT ''");
			}
			if (!has_receiver_text) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN receiver_text "
				     "TEXT DEFAULT ''");
			}
			if (!has_receiver_type) {
				exec("ALTER TABLE semantic_records "
				     "ADD COLUMN receiver_type "
				     "TEXT DEFAULT ''");
			}
			if (!has_import_alias) {
				exec("ALTER TABLE semantic_records "
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
					exec("ALTER TABLE reference ADD COLUMN "
					     "qualified_target TEXT DEFAULT ''");
				if (!has_receiver_text)
					exec("ALTER TABLE reference ADD COLUMN "
					     "receiver_text TEXT DEFAULT ''");
				if (!has_receiver_type)
					exec("ALTER TABLE reference ADD COLUMN "
					     "receiver_type TEXT DEFAULT ''");
				if (!has_import_alias)
					exec("ALTER TABLE reference ADD COLUMN "
					     "import_alias TEXT DEFAULT ''");
				if (!has_call_site_file)
					exec("ALTER TABLE reference ADD COLUMN "
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
					exec("ALTER TABLE relation ADD COLUMN "
					     "confidence REAL DEFAULT 0.0");
				if (!has_resolver)
					exec("ALTER TABLE relation ADD COLUMN "
					     "resolver TEXT DEFAULT ''");
				if (!has_res_kind)
					exec("ALTER TABLE relation ADD COLUMN "
					     "resolution_kind TEXT DEFAULT ''");
				if (!has_reason)
					exec("ALTER TABLE relation ADD COLUMN "
					     "reason TEXT DEFAULT ''");
				if (!has_csf)
					exec("ALTER TABLE relation ADD COLUMN "
					     "call_site_file TEXT DEFAULT ''");
				if (!has_csr)
					exec("ALTER TABLE relation ADD COLUMN "
					     "call_site_row INTEGER DEFAULT 0");
				if (!has_csc)
					exec("ALTER TABLE relation ADD COLUMN "
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
				exec("CREATE TABLE IF NOT EXISTS type_info ("
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
				exec("CREATE INDEX IF NOT EXISTS idx_ti_name "
				     "ON type_info(project_id, name)");
				exec("CREATE INDEX IF NOT EXISTS idx_ti_qn "
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
				exec("CREATE TABLE IF NOT EXISTS type_ref ("
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
				exec("CREATE INDEX IF NOT EXISTS idx_tr_type "
				     "ON type_ref(project_id, type_name)");
				exec("CREATE INDEX IF NOT EXISTS idx_tr_entity "
				     "ON type_ref(project_id, entity_id)");
			} else {
				sqlite3_finalize(probe);
			}
		}
	}

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
				exec("ALTER TABLE module_summary "
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
				exec("ALTER TABLE entity "
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
				if (!exec("ALTER TABLE entity "
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
				exec("ALTER TABLE reference "
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
				exec("ALTER TABLE reference "
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
				exec("ALTER TABLE graph_nodes "
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
				exec("ALTER TABLE graph_edges "
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

	return true;
}

} // namespace store
