#include "store.h"
#include "store_internal.h"
#include "store_graph_compiler.h"
#include "platform_win.h"
#include "../resolver/pipeline.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <string>

// ─── RecordKind named constants ─────────────────────────────────
// Must match the definitions in semantic_unit.h. If the enum changes,
// update these constants to keep SQL queries in sync.
// Only the kinds used in SQL queries are listed here.
// Reference: engine/src/ir/semantic_unit.h enum class RecordKind
constexpr int kKindFunction = 0;
constexpr int kKindMethod = 1;
constexpr int kKindClass = 2;
constexpr int kKindInterface = 3;
constexpr int kKindEnum = 4;
constexpr int kKindTypeAlias = 5;
constexpr int kKindTypeDecl = 16;
constexpr int kKindTypeRef = 17;
constexpr int kKindTypeAssign = 18;
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

#include "../graph/graph_builder.h"
#include "../ir/semantic_unit.h"

// ── Batch import helper ─────────────────────────────────────────
namespace store
{

// ── Batch import helper ─────────────────────────────────────────
struct PathRec {
	std::string path;
	std::string alias;
	std::string file;
};

static void flushImportBatch(sqlite3 *db, uint64_t project_id,
			     const std::vector<PathRec> &batch)
{
	if (batch.empty())
		return;
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
		return;
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
	if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT)
		fprintf(stderr,
			"[module=store, method=flushImportBatch] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);
}

bool GraphStore::buildGraph(uint64_t project_id, bool build_calls,
			    const std::unordered_set<std::string> *changed_files)
{
	using Clock = std::chrono::steady_clock;

	// Increase SQLite cache to 256MB to reduce disk I/O during buildGraph.
	// The default 2MB cache is too small for large projects (45053+ nodes),
	// causing frequent page cache misses. 256MB fits comfortably in memory
	// for projects up to ~1M nodes. Use negative value to specify KB.
	exec("PRAGMA cache_size = -262144");

	// Incremental vs full rebuild: when changed_files is non-null, only
	// a subset of files are re-indexed — lookup indexes stay valid.
	const bool full_rebuild = (changed_files == nullptr);

	// Step 1: determine which files to rebuild
	auto t0 = Clock::now();
	std::string file_list_sql =
		"SELECT DISTINCT file_path FROM semantic_records WHERE project_id=" +
		std::to_string(project_id);
	sqlite3_stmt *fl_stmt = nullptr;
	if (sqlite3_prepare_v2(db_, file_list_sql.c_str(), -1, &fl_stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"buildGraph: prepare file_list failed: %s "
			"[module=store, method=buildGraph]\n",
			sqlite3_errmsg(db_));
		// Fall through: empty rebuild_files is a no-op, not a fatal error.
		// The transaction is still committed below.
	}

	std::vector<std::string> rebuild_files;
	if (fl_stmt) {
		while (sqlite3_step(fl_stmt) == SQLITE_ROW) {
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(fl_stmt, 0));
			if (!fp)
				continue;
			std::string file_path(fp);
			if (changed_files && changed_files->find(file_path) ==
						     changed_files->end())
				continue;
			rebuild_files.push_back(std::move(file_path));
		}
		sqlite3_finalize(fl_stmt);
	}

	// Wrap the entire buildGraph in a transaction so that a crash mid-way
	// leaves the graph in a consistent state (all or nothing).
	// NOTE: engine_index_project.cpp already wraps buildGraph in a
	// transaction, so this is a no-op when called from there. Use SAVEPOINT
	// to handle nested calls safely.
	exec("SAVEPOINT buildGraph");
	auto t_file_list = Clock::now();

	if (rebuild_files.empty()) {
		exec("RELEASE SAVEPOINT buildGraph");
		return true;
	}

	// Delete existing graph data for files being rebuilt.
	// deleteGraphDataByFile cleans entity, relation (graph_nodes/graph_edges are deprecated)
	// AND entity — the old code only deleted edges+nodes, leaving entity
	// rows that caused duplicate accumulation on re-index.
	for (auto &fp : rebuild_files) {
		deleteGraphDataByFile(project_id, fp.c_str());
	}
	auto t_delete = Clock::now();

	std::string pid = std::to_string(project_id);

	// ── 2a: Create file filter temp table ──
	exec("DROP TABLE IF EXISTS _rf");
	exec("CREATE TEMP TABLE _rf (file_path TEXT PRIMARY KEY)");
	{
		sqlite3_stmt *ins = nullptr;
		sqlite3_prepare_v2(
			db_, "INSERT OR IGNORE INTO _rf (file_path) VALUES (?)",
			-1, &ins, nullptr);
		for (auto &fp : rebuild_files) {
			sqlite3_bind_text(ins, 1, fp.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_step(ins);
			sqlite3_reset(ins);
		}
		sqlite3_finalize(ins);
	}
	auto t_rf = Clock::now();

	// ── 2b: Create _r2n mapping table (unsorted for speed) ──
	// Note: ROW_NUMBER() OVER () avoids ORDER BY sort cost.
	// Node IDs are sequential but not sorted by file_path — sorting is
	// not required for correctness since JOINs use indexes, not sequential scans.
	static constexpr int kR2nKinds[] = {
		kKindFunction,	kKindMethod,  kKindClass,
		kKindInterface, kKindEnum,    kKindTypeAlias,
		kKindTypeDecl,	kKindTypeRef, kKindTypeAssign,
	};
	static constexpr int kNumR2nKinds =
		sizeof(kR2nKinds) / sizeof(kR2nKinds[0]);

	// Build the kind list string for SQL IN clause
	std::string kind_list = "(";
	for (int i = 0; i < kNumR2nKinds; i++) {
		if (i > 0)
			kind_list += ",";
		kind_list += std::to_string(kR2nKinds[i]);
	}
	kind_list += ")";

	exec("DROP TABLE IF EXISTS _r2n");
	exec(std::string(
		     "CREATE TEMP TABLE _r2n AS "
		     "SELECT sr.rowid as rid, sr.original_id, sr.file_path, sr.name,"
		     " CAST(ROW_NUMBER() OVER () AS INTEGER) as node_id "
		     "FROM semantic_records sr "
		     "WHERE sr.project_id=" +
		     pid + " AND sr.kind IN " + kind_list +
		     " AND sr.name != ''"
		     " AND sr.file_path IN (SELECT file_path FROM _rf)")
		     .c_str());
	const char *explain_env = getenv("CODESCOPE_EXPLAIN");
	if (explain_env && explain_env[0]) {
		explainQueryPlan(
			(std::string(
				 "CREATE TEMP TABLE _r2n AS "
				 "SELECT sr.rowid as rid, sr.original_id, sr.file_path, sr.name,"
				 " CAST(ROW_NUMBER() OVER () AS INTEGER) as node_id "
				 "FROM semantic_records sr "
				 "WHERE sr.project_id=" +
				 pid + " AND sr.kind IN " + kind_list +
				 " AND sr.name != ''"
				 " AND sr.file_path IN (SELECT file_path FROM _rf)")
				 .c_str()),
			"_r2n");
	}
	exec("CREATE INDEX IF NOT EXISTS _r2n_fp_oid ON _r2n(file_path, original_id)");
	exec("CREATE INDEX IF NOT EXISTS _r2n_name ON _r2n(name)");
	// Index for type_ref JOIN: sr.rowid = r2n.rid speeds up the
	// INSERT INTO type_ref ... FROM semantic_records JOIN _r2n query.
	exec("CREATE INDEX IF NOT EXISTS _r2n_rid ON _r2n(rid)");
	auto t_r2n = Clock::now();

	// ── 2c: Graph nodes from declarations ──

	if (explain_env && explain_env[0]) {
		explainQueryPlan(
			"SELECT r2n.node_id, sr.project_id, sr.original_id, "
			" CASE sr.kind WHEN 0 THEN 0 WHEN 1 THEN 1 WHEN 2 THEN 2 "
			"  WHEN 3 THEN 4 WHEN 4 THEN 3 WHEN 5 THEN 3 ELSE 7 END, "
			" sr.name, COALESCE(NULLIF(sr.qualified_name, ''), sr.name), COALESCE(NULLIF(sr.qualified_name, ''), sr.name), sr.file_path, sr.file_path, "
			" sr.start_row, sr.start_col, sr.end_row, sr.end_col, sr.language "
			"FROM semantic_records sr JOIN _r2n r2n ON sr.rowid = r2n.rid",
			"nodes");
	}
	// graph_nodes table is deprecated. Entity data is written below.
	// Phase 1.1: dual-write to entity table (with test-file filter).
	// Filter test/bench/spec files — AI only needs production code.
	// This early insert feeds the scope table below, so it must happen
	// before scope creation. The late insert after dropQueryIndexes is
	// now redundant but kept as a safety net (INSERT OR IGNORE).
	// Includes sr.arity so the Resolver Pipeline can disambiguate
	// same-name overloads via factorSignatureMatch. See
	// CODE_REVIEW_FINDINGS_2026-07-19.md C2.
	exec(std::string(
		     "INSERT OR IGNORE INTO entity "
		     "(id, project_id, kind, name, qualified_name, "
		     " file_path, language, start_row, start_col, "
		     " end_row, end_col, module_path, visibility, arity) "
		     "SELECT r2n.node_id, sr.project_id, "
		     " CASE sr.kind WHEN 0 THEN 0 WHEN 1 THEN 1 WHEN 2 THEN 2 "
		     "  WHEN 3 THEN 4 WHEN 4 THEN 3 WHEN 5 THEN 3 ELSE 7 END, "
		     " sr.name, COALESCE(NULLIF(sr.qualified_name, ''), sr.name), "
		     " sr.file_path, sr.language, "
		     " sr.start_row, sr.start_col, sr.end_row, sr.end_col, "
		     " rtrim(sr.file_path, replace(sr.file_path, '/', 'x')), "
		     " sr.visibility, "
		     " sr.arity "
		     "FROM semantic_records sr "
		     "JOIN _r2n r2n ON sr.rowid = r2n.rid "
		     "WHERE sr.file_path NOT LIKE '%\\_test.%' ESCAPE '\\'"
		     " AND sr.file_path NOT LIKE '%/tests/%'"
		     " AND sr.file_path NOT LIKE '%\\_spec.%' ESCAPE '\\'"
		     " AND sr.file_path NOT LIKE '%/benches/%'"
		     " AND sr.file_path NOT LIKE '%\\_\\_test\\_\\_%' ESCAPE '\\'")
		     .c_str());
	auto t_nodes = Clock::now();

	// ── 2d: Containment edges (edge_type=3) ──
	// graph_edges is deprecated. Containment relationships are
	// derived from entity parent_id at query time via LadybugDB.
	// The old INSERT INTO graph_edges for containment is removed.
	auto t_edges = Clock::now();

	// ── 2e: Route + type edges + type_info + type_ref ──
	// Phase 3: Use SQL-based call edge resolution instead of C++ hash maps.
	// The old approach loaded all declarations into in-memory hash maps
	// (caller_idx, callee_by_name, callee_by_short, decl_idx, ir_edge_target),
	// consuming ~2-4GB for 4M nodes. The new approach uses SQL temp tables
	// with indexes, keeping peak memory bounded by SQLite's cache (~64MB).
	// P3 HashMap (buildCallEdgesSQL) has been replaced by the new
	// Resolver Pipeline (Phase 1.3) with multi-factor scoring.
	(void)build_calls;

	// Phase 1.3: Type edges — create USES_TYPE edges from TypeRef records
	{
		// Populate route table from Route records (kind=19 in semantic_records)
		exec(std::string(
			     "INSERT OR IGNORE INTO route "
			     "(project_id, method, path, handler_name, "
			     " file_path, start_row, start_col) "
			     "SELECT sr.project_id, "
			     " SUBSTR(sr.name, 1, INSTR(sr.name, ' ') - 1), "
			     " SUBSTR(sr.name, INSTR(sr.name, ' ') + 1), "
			     " sr.qualified_name, "
			     " sr.file_path, sr.start_row, sr.start_col "
			     "FROM semantic_records sr "
			     "WHERE sr.project_id=" +
			     pid +
			     " AND sr.kind = 19" // Route
			     " AND sr.name != '' AND sr.name LIKE '% %'"
			     " AND sr.file_path IN (SELECT file_path FROM _rf)")
			     .c_str());
	}
	auto t_route = Clock::now();

	// Build the type kind list for SQL IN clause.
	// Visitors emit type declarations as Class/Interface/Enum/TypeAlias,
	// not TypeDecl. Use these kinds directly to avoid a fragile
	// find+replace that would silently fail if the SQL changes.
	{
		std::string type_kind_list =
			"(" + std::to_string(kKindClass) + "," +
			std::to_string(kKindInterface) + "," +
			std::to_string(kKindEnum) + "," +
			std::to_string(kKindTypeAlias) + ")";

		// Materialize type declarations into a temp table so the type_ref
		// JOIN below scans _td (project-local, kind-filtered) instead of
		// semantic_records twice. Avoids the second full-table scan that
		// previously dominated the type_edges stage.
		exec("DROP TABLE IF EXISTS _td");
		exec(std::string(
			     "CREATE TEMP TABLE _td AS "
			     "SELECT sr.rowid as rid, sr.name, sr.file_path, sr.kind "
			     "FROM semantic_records sr "
			     "WHERE sr.project_id=" +
			     pid + " AND sr.kind IN " + type_kind_list +
			     " AND sr.name != ''")
			     .c_str());
		// _td_rid covers the JOIN `_td.rid = tgt.rid AND _td.name = sr.type_name`:
		// B-tree leftmost-prefix means (rid, name) supports rid-only AND
		// rid+name lookups, matching the JOIN condition exactly. The
		// previous (name, rid) index was unusable for this JOIN because
		// the lookup starts from tgt.rid, not from name.
		exec("CREATE INDEX IF NOT EXISTS _td_rid ON _td(rid, name)");
		exec("CREATE INDEX IF NOT EXISTS _td_name ON _td(name, rid)");

		// graph_edges type edges are deprecated. Type relationships
		// are now derived from type_info + type_ref at query time.
	}
	auto t_type_edges = Clock::now();

	// Populate type_info table from type declaration records.
	{
		std::string type_kind_list =
			"(" + std::to_string(kKindClass) + "," +
			std::to_string(kKindInterface) + "," +
			std::to_string(kKindEnum) + "," +
			std::to_string(kKindTypeAlias) + ")";
		exec(std::string(
			     "INSERT OR IGNORE INTO type_info "
			     "(project_id, name, qualified_name, kind, "
			     " file_path, language, start_row, start_col, "
			     " end_row, end_col) "
			     "SELECT sr.project_id, sr.name, "
			     " COALESCE(NULLIF(sr.qualified_name, ''), sr.name), "
			     " CASE sr.kind"
			     "  WHEN " +
			     std::to_string(kKindClass) +
			     " THEN 0 "
			     "  WHEN " +
			     std::to_string(kKindEnum) +
			     " THEN 1 "
			     "  WHEN " +
			     std::to_string(kKindInterface) +
			     " THEN 3 "
			     "  WHEN " +
			     std::to_string(kKindTypeAlias) +
			     " THEN 4 "
			     "  ELSE 0 END, "
			     " sr.file_path, sr.language, "
			     " sr.start_row, sr.start_col, sr.end_row, sr.end_col "
			     "FROM semantic_records sr "
			     "WHERE sr.project_id=" +
			     pid + " AND sr.kind IN " + type_kind_list +
			     " AND sr.name != ''"
			     " AND sr.file_path IN (SELECT file_path FROM _rf)")
			     .c_str());
	}
	auto t_type_info = Clock::now();

	// Populate type_ref table from TypeRef records.
	{
		exec(std::string(
			     "INSERT OR IGNORE INTO type_ref "
			     "(project_id, entity_id, type_name, kind, "
			     " file_path, start_row, start_col) "
			     "SELECT sr.project_id, r2n.node_id, sr.type_name, "
			     " CASE WHEN sr.name LIKE '%.return' THEN 2 ELSE 0 END, "
			     " sr.file_path, sr.start_row, sr.start_col "
			     "FROM semantic_records sr "
			     "JOIN _r2n r2n ON sr.rowid = r2n.rid "
			     "WHERE sr.project_id=" +
			     pid +
			     " AND sr.kind = " + std::to_string(kKindTypeRef) +
			     " AND sr.name != '' AND sr.type_name != ''"
			     " AND sr.file_path IN (SELECT file_path FROM _rf)")
			     .c_str());
	}
	auto t_type_ref = Clock::now();

	// ── P0.5: CSR build moved to AFTER resolver ──
	// Previously buildCSR ran here, before the resolver inserted call
	// edges. The CSR (adjacency BLOBs) therefore missed all resolver-
	// generated call edges, making caller/callee lookups incomplete.
	// CSR is now built after the resolver pipeline (see below).

	// Phase 1.2: populate reference table from semantic_records CallExpr ONLY.
	// Uses _r2n mapping to resolve parent_id -> caller_id.
	//
	// FIX (FP edge elimination): previously this also included MemberExpr
	// (kind=10, field/property accesses such as `params.agentId`). The
	// ResolverPipeline (Phase 1.3) fuzzy-resolves EVERY row in `reference`
	// as a CALL and emits a `Calls` edge (edge_type=1). A MemberExpr like
	// `params.agentId` is NOT a call, yet could be loosely bound (prefix/
	// suffix LIKE) to an unrelated function — e.g. getMemoryThreads's
	// `params.agentId` member access resolving to `getAgent`, producing a
	// false-positive `getMemoryThreads -> getAgent` edge with
	// graph_type='symbol_reference'. MemberExpr field accesses are therefore
	// excluded from the call-reference table (the `reference` table's
	// documented purpose is "call facts recorded by Parser"). Field/property
	// accesses that legitimately need a symbol-reference edge should be added
	// by a dedicated pass that emits `References` edges via EXACT match, not
	// by the fuzzy call resolver.
	//
	// Note: prior to this fix, the kind=10 inclusion was justified as
	// "capturing struct method calls like a.submitToCoordinator(ctx)", but
	// the fuzzy resolver matches the FULL member path (e.g.
	// "a.submitToCoordinator") which never equals the bare method entity
	// ("submitToCoordinator"), so it produced no correct edges — only FPs.
	{
		std::string ref_sql =
			"INSERT OR IGNORE INTO reference "
			"(project_id, caller_id, name, arity, call_kind, "
			" resolve_strategy, start_row, start_col, "
			// Step 3 (plan §3.1): structured call facts copied from
			// semantic_records so the Resolver can disambiguate
			// method/static/constructor calls with structured evidence.
			" qualified_target, receiver_text, receiver_type, "
			" import_alias, call_site_file) "
			"SELECT sr.project_id, r2n.node_id, sr.name, sr.arity, "
			" sr.call_kind, sr.resolve_strategy, "
			" sr.start_row, sr.start_col, "
			" sr.qualified_target, sr.receiver_text, "
			" sr.receiver_type, sr.import_alias, sr.file_path "
			"FROM semantic_records sr "
			"JOIN _r2n r2n ON sr.parent_id = r2n.original_id "
			" AND sr.file_path = r2n.file_path "
			"WHERE sr.project_id=" +
			std::to_string(project_id) +
			" AND sr.kind = 9 AND sr.name != '' AND sr.file_path NOT LIKE '%\\_test.%' ESCAPE '\\' AND sr.file_path NOT LIKE '%/tests/%' AND sr.file_path NOT LIKE '%\\_spec.%' ESCAPE '\\' AND sr.file_path NOT LIKE '%/benches/%' AND sr.file_path NOT LIKE '%\\_\\_test\\_\\_%' ESCAPE '\\'"
			" AND sr.file_path IN (SELECT file_path FROM _rf)";
		exec(ref_sql.c_str());
	}
	auto t_reference = Clock::now();

	// Phase 1.2: populate import table from semantic_records Import
	// Parse raw import text to extract individual import paths.
	{
		const char *fetch_sql =
			"SELECT sr.name, sr.project_id, sr.file_path FROM semantic_records sr "
			"WHERE sr.project_id=? AND sr.kind=11 AND sr.name != '' AND sr.file_path NOT LIKE '%\\_test.%' ESCAPE '\\' AND sr.file_path NOT LIKE '%/tests/%' AND sr.file_path NOT LIKE '%\\_spec.%' ESCAPE '\\' AND sr.file_path NOT LIKE '%/benches/%'"
			" AND sr.file_path IN (SELECT file_path FROM _rf)";
		sqlite3_stmt *fetch_st = nullptr;
		if (sqlite3_prepare_v2(db_, fetch_sql, -1, &fetch_st,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(fetch_st, 1,
					   static_cast<int64_t>(project_id));

			while (sqlite3_step(fetch_st) == SQLITE_ROW) {
				const char *raw = reinterpret_cast<const char *>(
					sqlite3_column_text(fetch_st, 0));
				if (!raw)
					continue;
				std::string text(raw);
				std::vector<std::string> paths;
				size_t pos = 0;
				while ((pos = text.find_first_of("\"'", pos)) !=
				       std::string::npos) {
					char q = text[pos];
					size_t end = text.find(q, pos + 1);
					if (end == std::string::npos)
						break;
					std::string p = text.substr(
						pos + 1, end - pos - 1);
					if (!p.empty() &&
					    p.find('.') != std::string::npos)
						paths.push_back(p);
					pos = end + 1;
				}
				size_t use_pos = 0;
				while ((use_pos = text.find("use ", use_pos)) !=
				       std::string::npos) {
					size_t semi = text.find(';', use_pos);
					if (semi == std::string::npos)
						break;
					std::string p =
						text.substr(use_pos + 4,
							    semi - use_pos - 4);
					p.erase(0, p.find_first_not_of(" \t"));
					p.erase(p.find_last_not_of(" \t") + 1);
					if (!p.empty() &&
					    p.find("::") != std::string::npos)
						paths.push_back(p);
					use_pos = semi + 1;
				}
				size_t from_pos = 0;
				while ((from_pos =
						text.find("from ", from_pos)) !=
				       std::string::npos) {
					size_t imp =
						text.find(" import ", from_pos);
					if (imp == std::string::npos)
						break;
					std::string p =
						text.substr(from_pos + 5,
							    imp - from_pos - 5);
					p.erase(0, p.find_first_not_of(" \t"));
					p.erase(p.find_last_not_of(" \t") + 1);
					if (!p.empty())
						paths.push_back(p);
					from_pos = imp + 8;
				}
				std::vector<PathRec> batch;
				constexpr size_t kMaxBatch = 500;
				for (auto &p : paths) {
					PathRec pr;
					pr.path = p;
					size_t last_sep = p.rfind('/');
					if (last_sep == std::string::npos)
						last_sep = p.rfind("::");
					pr.alias =
						(last_sep !=
						 std::string::npos) ?
							p.substr(last_sep + 1) :
							p;
					const char *fp_c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								fetch_st, 2));
					pr.file = fp_c ? fp_c : "";
					batch.push_back(pr);
					if (batch.size() >= kMaxBatch) {
						flushImportBatch(
							db_, project_id, batch);
						batch.clear();
					}
				}
				if (!batch.empty())
					flushImportBatch(db_, project_id,
							 batch);
			}
			sqlite3_finalize(fetch_st);
		}
	}
	auto t_import = Clock::now();

	// Phase 1.2: populate scope table from entity module_path.
	// Module scopes: one per unique directory (module_path is the
	// denormalized directory portion of file_path, populated at INSERT).
	{
		std::string scope_sql =
			"INSERT OR IGNORE INTO scope "
			"(project_id, parent_id, kind, name, start_row, end_row) "
			"SELECT DISTINCT " +
			std::to_string(project_id) +
			", 0, 1, module_path, "
			"0, 0 "
			"FROM entity WHERE project_id=" +
			std::to_string(project_id) +
			" AND module_path != ''"
			" AND entity.file_path IN (SELECT file_path FROM _rf)";
		exec(scope_sql.c_str());
	}
	// Function scopes: each entity within its module scope.
	{
		std::string func_sql =
			"INSERT OR IGNORE INTO scope "
			"(project_id, parent_id, kind, name, start_row, end_row) "
			"SELECT " +
			std::to_string(project_id) +
			", s.id, 2, e.name, "
			" e.start_row, e.end_row "
			"FROM entity e "
			"JOIN scope s ON e.project_id = s.project_id"
			" AND s.kind = 1"
			" AND s.name = e.module_path "
			"WHERE e.project_id=" +
			std::to_string(project_id) +
			" AND e.file_path IN (SELECT file_path FROM _rf)";
		exec(func_sql.c_str());
	}
	// Update import.source_scope_id to point to the file's module scope.
	{
		std::string imp_scope_sql =
			"UPDATE import SET source_scope_id = "
			"(SELECT COALESCE(s.id, 0) FROM scope s "
			" JOIN entity e ON e.project_id = s.project_id"
			" AND s.kind = 1"
			" AND s.name = e.module_path "
			" WHERE e.project_id=import.project_id"
			" AND e.file_path = "
			"  (SELECT file_path FROM semantic_records sr"
			"   WHERE sr.rowid = import.id"
			"   AND sr.project_id=import.project_id)"
			" LIMIT 1) "
			"WHERE project_id=" +
			std::to_string(project_id);
		exec(imp_scope_sql.c_str());
	}
	auto t_scope = Clock::now();

	exec("DROP TABLE IF EXISTS _r2n");
	exec("DROP TABLE IF EXISTS _rf");
	exec("DROP TABLE IF EXISTS _td");

	// ── P3: Drop query indexes before bulk edge inserts ──────────
	// Full rebuild: drop all lookup + unique indexes for max insert
	// throughput, recreate via createIndexesAfterBulkLoad(full_rebuild=true).
	// Incremental: only drop idx_ge_unique_edge so INSERT OR IGNORE
	// skips the unique-check cost. Lookup indexes stay valid and are
	// maintained automatically by SQLite during incremental inserts.
	if (full_rebuild) {
		dropQueryIndexes();
	} else {
		dropUniqueEdgeIndex();
	}

	// Phase 1.1: entity/relation are already written above from
	// semantic_records (entity) and by the resolver pipeline (relation).
	// The old dual-write from graph_nodes/graph_edges is removed.
	auto t_entity_relation = Clock::now();

	// Phase 1.3: Resolver Pipeline — resolve references to entities
	// Inserts resolved call edges into relation + graph_edges via a
	// staging table (P1 batch optimization).
	{
		resolver::ResolverPipeline pipe(this, project_id);
		int64_t resolved = pipe.run();
		// pipe.run() returns -1 on prepare failure (it does NOT throw).
		// The previous code ignored the return value, so a resolver
		// failure was silently treated as success: no resolved edges
		// were produced but buildGraph continued to buildCSR and
		// returned true, and the SAVEPOINT was RELEASEd — losing the
		// "all or nothing" guarantee for the resolver phase. Now we
		// ROLLBACK TO SAVEPOINT and propagate the failure.
		if (resolved < 0) {
			fprintf(stderr,
				"buildGraph: resolver pipeline failed (rc=%lld)"
				" for project %s — rolling back savepoint "
				"[module=store, method=buildGraph]\n",
				(long long)resolved, pid.c_str());
			exec("ROLLBACK TO SAVEPOINT buildGraph");
			exec("RELEASE SAVEPOINT buildGraph");
			return false;
		}
	}
	auto t_resolver = Clock::now();

	// ── P0.5: Build CSR AFTER resolver ────────────────────────────
	// Now that the resolver has inserted all call edges into
	// graph_edges, build CSR adjacency BLOBs so they include resolver-
	// generated edges. Previously CSR was built before the resolver,
	// missing all resolved call edges.
	if (build_calls) {
		if (!buildCSR(project_id)) {
			fprintf(stderr,
				"buildGraph: buildCSR failed for "
				"project %s [module=store, method=buildGraph]\n",
				pid.c_str());
		}
	}
	auto t_csr = Clock::now();

	// ── P3: Model Engine + State Builder moved to async path ──────
	// Previously model/state ran synchronously inside buildGraph, blocking
	// the user from querying until all high-level models were built.
	// Now buildGraph returns after CSR (the core graph is complete and
	// queryable), and model/state run in a background thread launched by
	// engine_index_project.cpp after createIndexesAfterBulkLoad.

	// ── Build LadybugDB from entity/relation tables ──
	// Builds the Cypher-queryable graph from the canonical entity/relation
	// tables. Non-fatal: if the build fails, isGraphReady() returns false
	// and all query paths return "graph not ready" errors.

	auto t_lbug = Clock::now();
	if (lbug_initialized_) {
		if (!buildLadybugFromEntityRelation(this, project_id)) {
			fprintf(stderr,
				"buildGraph: buildLadybugFromEntityRelation failed "
				"for project %s — SQLite graph remains the "
				"source of truth "
				"[module=store, method=buildGraph]\n",
				pid.c_str());
		}
	}
	fprintf(stderr,
		"buildGraph: ladybugdb=%lldms "
		"for project %s\n",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - t_lbug)
			.count(),
		pid.c_str());

	auto t_cleanup = Clock::now();

	// ── Step 0: Fine-grained phase timing breakdown ───────────────
	auto ms = [](auto start, auto end) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			       end - start)
			.count();
	};
	fprintf(stderr,
		"buildGraph: %zu files"
		" | file_list=%lldms delete=%lldms rf=%lldms r2n=%lldms"
		" nodes=%lldms edges=%lldms route=%lldms"
		" type_edges=%lldms type_info=%lldms type_ref=%lldms"
		" ref=%lldms import=%lldms scope=%lldms"
		" ent_rel=%lldms resolver=%lldms csr=%lldms"
		" cleanup=%lldms total=%lldms\n",
		rebuild_files.size(), (long long)ms(t0, t_file_list),
		(long long)ms(t_file_list, t_delete),
		(long long)ms(t_delete, t_rf), (long long)ms(t_rf, t_r2n),
		(long long)ms(t_r2n, t_nodes), (long long)ms(t_nodes, t_edges),
		(long long)ms(t_edges, t_route),
		(long long)ms(t_route, t_type_edges),
		(long long)ms(t_type_edges, t_type_info),
		(long long)ms(t_type_info, t_type_ref),
		(long long)ms(t_type_ref, t_reference),
		(long long)ms(t_reference, t_import),
		(long long)ms(t_import, t_scope),
		(long long)ms(t_scope, t_entity_relation),
		(long long)ms(t_entity_relation, t_resolver),
		(long long)ms(t_resolver, t_csr),
		(long long)ms(t_csr, t_cleanup), (long long)ms(t0, t_cleanup));

	exec("RELEASE SAVEPOINT buildGraph");
	return true;
}

// ── CSR Adjacency (BLOB-packed call edges) ─────────────────────

bool GraphStore::buildCSR(uint64_t project_id)
{
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
	if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
		return false;

	const char *ins_sql =
		"INSERT OR REPLACE INTO adjacency (src_id, project_id, tgt_blob) "
		"VALUES (?, ?, ?)";
	sqlite3_stmt *ins = nullptr;
	if (sqlite3_prepare_v2(db_, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
		sqlite3_finalize(st);
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
	    SQLITE_OK)
		return false;

	const char *rev_ins_sql =
		"INSERT OR REPLACE INTO adjacency_rev (tgt_id, project_id, "
		"src_blob) VALUES (?, ?, ?)";
	sqlite3_stmt *rev_ins = nullptr;
	if (sqlite3_prepare_v2(db_, rev_ins_sql, -1, &rev_ins, nullptr) !=
	    SQLITE_OK) {
		sqlite3_finalize(rev_st);
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
	return true;
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
		const void *blob = sqlite3_column_blob(st, 0);
		int bytes = sqlite3_column_bytes(st, 0);
		int n = bytes / static_cast<int>(sizeof(uint64_t));
		const uint64_t *arr = static_cast<const uint64_t *>(blob);
		ids.reserve(static_cast<size_t>(n));
		for (int i = 0; i < n; i++)
			ids.push_back(static_cast<uint64_t>(arr[i]));
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
		const void *blob = sqlite3_column_blob(st, 0);
		int bytes = sqlite3_column_bytes(st, 0);
		int n = bytes / static_cast<int>(sizeof(uint64_t));
		const uint64_t *arr = static_cast<const uint64_t *>(blob);
		ids.reserve(static_cast<size_t>(n));
		for (int i = 0; i < n; i++)
			ids.push_back(static_cast<uint64_t>(arr[i]));
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
		const void *blob = sqlite3_column_blob(st, 1);
		int bytes = sqlite3_column_bytes(st, 1);
		int n = bytes / static_cast<int>(sizeof(uint64_t));
		const uint64_t *arr = static_cast<const uint64_t *>(blob);
		uint64_t target = node_id;
		for (int i = 0; i < n; i++) {
			if (arr[i] == target) {
				ids.push_back(static_cast<uint64_t>(src));
				break;
			}
		}
	}
	sqlite3_finalize(st);
	return ids;
}

// ── BulkPragmaGuard: RAII bulk-load PRAGMA tuning ───────────────
// Matches codebase-memory-mcp cbm_store_begin_bulk/end_bulk. Saves
// synchronous + cache_size, sets OFF + 64 MB, restores on destruction.
// SQLite cache_size is in KiB when negative. -65536 = 64 MiB.
constexpr int kBulkCacheSizeKib = -65536;
GraphStore::BulkPragmaGuard::BulkPragmaGuard(GraphStore *store)
	: store_(store)
{
	if (!store_)
		return;
	auto read_pragma = [](sqlite3 *db, const char *name) {
		sqlite3_stmt *st = nullptr;
		int val = 0;
		std::string sql = std::string("PRAGMA ") + name;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) ==
		    SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW)
				val = sqlite3_column_int(st, 0);
			sqlite3_finalize(st);
		}
		return val;
	};
	sqlite3 *db = store_->handle();
	if (!db) {
		fprintf(stderr, "[module=store, method=BulkPragmaGuard] "
				"db handle is null, cannot save/set PRAGMAs\n");
		return;
	}
	saved_sync_ = read_pragma(db, "synchronous");
	saved_cache_ = read_pragma(db, "cache_size");
	if (!store_->exec("PRAGMA synchronous = OFF"))
		fprintf(stderr,
			"[module=store, method=BulkPragmaGuard] "
			"PRAGMA synchronous=OFF failed: %s\n",
			store_->error().c_str());
	if (!store_->exec(
		    ("PRAGMA cache_size = " + std::to_string(kBulkCacheSizeKib))
			    .c_str()))
		fprintf(stderr,
			"[module=store, method=BulkPragmaGuard] "
			"PRAGMA cache_size=%d failed: %s\n",
			kBulkCacheSizeKib, store_->error().c_str());
}
GraphStore::BulkPragmaGuard::~BulkPragmaGuard()
{
	if (!store_)
		return;
	auto restore = [&](const char *name, int val) {
		if (!store_->exec((std::string("PRAGMA ") + name + " = " +
				   std::to_string(val))
					  .c_str()))
			fprintf(stderr,
				"[module=store, method=~BulkPragmaGuard] "
				"PRAGMA %s=%d restore failed: %s\n",
				name, val, store_->error().c_str());
	};
	restore("synchronous", saved_sync_);
	restore("cache_size", saved_cache_);
}
} // namespace store
