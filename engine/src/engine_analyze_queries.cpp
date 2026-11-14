// engine_analyze_queries.cpp — the SQL reads behind analyze_project's views.
//
// Three rules decide this file:
//   1. a failed query returns `ok = false`, never an empty page: "no callers"
//      and "the query died" must not look the same (invariant 1, §17.5);
//   2. totals are counted, not copied from the truncated page (§18.7.0);
//   3. only `EdgeType::Calls` counts as a call edge. `Contains` (3) is a
//      class→method ownership edge and used to be mixed in, which made a class
//      look like a caller of its own methods.

#include "engine_analyze_queries.h"

#include "graph/graph_types.h"

#include <sqlite3.h>

#include <string>
#include <utility>
#include <vector>

namespace engine_views
{
namespace
{

/// The only relation type a caller/callee query may match. `Contains` (3) is
/// ownership, not a call (graph_types.h).
constexpr int kEdgeCalls = static_cast<int>(graph::EdgeType::Calls);

// ── Small SQL helpers ───────────────────────────────────────────

std::string textCol(sqlite3_stmt *stmt, int col)
{
	const char *text =
		reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
	return text ? text : "";
}

/// Run a COUNT statement with an int (type) and an int64 (entity id)
/// after project_id. The relation count SQL has three parameters:
/// `project_id=? AND type=? AND source_id=?` — a helper that only
/// binds two would put the entity id in the type slot and leave the
/// id slot unbound, silently returning total=0.
int64_t countWithIntAndInt64(sqlite3 *db, const char *sql, uint64_t project_id,
			     int type_val, int64_t entity_id, bool *ok)
{
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		*ok = false;
		return -1;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, type_val);
	sqlite3_bind_int64(stmt, 3, entity_id);
	int64_t total = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		total = sqlite3_column_int64(stmt, 0);
	else
		*ok = false;
	sqlite3_finalize(stmt);
	return total;
}

// ── Entity queries ──────────────────────────────────────────────

EntityRow readEntityRow(sqlite3_stmt *stmt)
{
	EntityRow r;
	r.id = sqlite3_column_int64(stmt, 0);
	r.name = textCol(stmt, 1);
	r.qualified_name = textCol(stmt, 2);
	r.kind = sqlite3_column_int(stmt, 3);
	r.file_path = textCol(stmt, 4);
	r.start_row = sqlite3_column_int64(stmt, 5);
	r.language = textCol(stmt, 6);
	r.module_path = textCol(stmt, 7);
	return r;
}

} // namespace

/// Resolve entities by bare or qualified name, optionally filtered by path.
Page<EntityRow> resolveEntities(sqlite3 *db, uint64_t project_id,
				const std::string &name,
				const std::string &file_filter)
{
	Page<EntityRow> page;
	const bool has_filter = !file_filter.empty();
	std::string where =
		"WHERE project_id=? AND (name=? OR qualified_name=?)";
	if (has_filter)
		where += " AND file_path LIKE ?";

	std::string select =
		"SELECT id, name, qualified_name, kind, file_path, "
		"       start_row, language, module_path FROM entity " +
		where + " ORDER BY id LIMIT ?";
	std::string count = "SELECT COUNT(*) FROM entity " + where;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, select.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
	int next = 4;
	if (has_filter) {
		std::string pattern = "%" + file_filter + "%";
		sqlite3_bind_text(stmt, next++, pattern.c_str(), -1,
				  SQLITE_TRANSIENT);
	}
	sqlite3_bind_int(stmt, next, kMaxResolvedEntities);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
		page.rows.push_back(readEntityRow(stmt));
	if (rc != SQLITE_DONE)
		page.ok = false;
	sqlite3_finalize(stmt);

	// The count repeats the predicate; it is not derived from the page.
	sqlite3_stmt *counter = nullptr;
	if (sqlite3_prepare_v2(db, count.c_str(), -1, &counter, nullptr) !=
	    SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(counter, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(counter, 2, name.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(counter, 3, name.c_str(), -1, SQLITE_TRANSIENT);
	if (has_filter) {
		std::string pattern = "%" + file_filter + "%";
		sqlite3_bind_text(counter, 4, pattern.c_str(), -1,
				  SQLITE_TRANSIENT);
	}
	if (sqlite3_step(counter) == SQLITE_ROW)
		page.total = sqlite3_column_int64(counter, 0);
	else
		page.ok = false;
	sqlite3_finalize(counter);
	return page;
}

/// Entities in a module (module_path prefix). `module_path` is already
/// normalized by parseScope, so no double separator can appear here.
///
/// The stored `entity.module_path` is absolute (the parser records the directory
/// it resolved), while a caller naturally passes a repository-relative path like
/// "engine/src/store". Matching only the literal value turned that into a
/// confident "0 entities" with source=fact, so both spellings are accepted: the
/// exact value, its '/'-suffixed form, its prefix, and — for a relative input —
/// its occurrence inside an absolute path.
Page<EntityRow> queryModuleEntities(sqlite3 *db, uint64_t project_id,
				    const std::string &module_path)
{
	Page<EntityRow> page;
	const char *where =
		"FROM entity WHERE project_id=? AND kind IN (0,1,2) "
		"AND (module_path=? OR module_path=? OR module_path LIKE ? "
		"     OR module_path LIKE ?)";
	const std::string select =
		std::string("SELECT id, name, qualified_name, kind, file_path, "
			    "       start_row, language, module_path ") +
		where + " ORDER BY kind, id LIMIT ?";
	const std::string count = std::string("SELECT COUNT(*) ") + where;
	const std::string with_slash = module_path + "/";
	const std::string prefix = module_path + "/%";
	const std::string anywhere = module_path.empty() ||
						     module_path[0] == '/' ?
					     std::string("\x01") :
					     "%/" + module_path + "/%";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, select.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, module_path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, with_slash.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, prefix.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, anywhere.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 6, kMaxModuleEntities);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
		page.rows.push_back(readEntityRow(stmt));
	if (rc != SQLITE_DONE)
		page.ok = false;
	sqlite3_finalize(stmt);

	sqlite3_stmt *counter = nullptr;
	if (sqlite3_prepare_v2(db, count.c_str(), -1, &counter, nullptr) !=
	    SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(counter, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(counter, 2, module_path.c_str(), -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_text(counter, 3, with_slash.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(counter, 4, prefix.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(counter, 5, anywhere.c_str(), -1, SQLITE_TRANSIENT);
	if (sqlite3_step(counter) == SQLITE_ROW)
		page.total = sqlite3_column_int64(counter, 0);
	else
		page.ok = false;
	sqlite3_finalize(counter);
	return page;
}

// ── Relation queries ────────────────────────────────────────────

/// Callers (`outgoing == false`) or callees (`outgoing == true`) of one entity.
///
/// Only `EdgeType::Calls` is matched: `Contains` means "this class defines that
/// method", and returning it as a caller made every class look like a consumer
/// of its own methods, polluting impact analysis.
Page<RelationRow> queryRelations(sqlite3 *db, uint64_t project_id,
				 int64_t entity_id, bool outgoing)
{
	Page<RelationRow> page;
	const char *sql =
		outgoing ?
			"SELECT COALESCE(NULLIF(e.qualified_name, ''), e.name), "
			"       e.file_path, e.start_row, r.type "
			"FROM relation r JOIN entity e ON e.id = r.target_id "
			"WHERE r.project_id=? AND r.type=? AND r.source_id=? "
			"GROUP BY e.id ORDER BY e.id LIMIT ?" :
			"SELECT COALESCE(NULLIF(e.qualified_name, ''), e.name), "
			"       e.file_path, e.start_row, r.type "
			"FROM relation r JOIN entity e ON e.id = r.source_id "
			"WHERE r.project_id=? AND r.type=? AND r.target_id=? "
			"GROUP BY e.id ORDER BY e.id LIMIT ?";
	const char *count_sql =
		outgoing ? "SELECT COUNT(DISTINCT target_id) FROM relation "
			   "WHERE project_id=? AND type=? AND source_id=?" :
			   "SELECT COUNT(DISTINCT source_id) FROM relation "
			   "WHERE project_id=? AND type=? AND target_id=?";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int(stmt, 2, kEdgeCalls);
	sqlite3_bind_int64(stmt, 3, entity_id);
	sqlite3_bind_int(stmt, 4, outgoing ? kMaxCallees : kMaxCallers);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		RelationRow r;
		r.entity_name = textCol(stmt, 0);
		r.file_path = textCol(stmt, 1);
		r.line = sqlite3_column_int64(stmt, 2);
		r.edge_type = sqlite3_column_int(stmt, 3);
		page.rows.push_back(std::move(r));
	}
	if (rc != SQLITE_DONE)
		page.ok = false;
	sqlite3_finalize(stmt);

	page.total = countWithIntAndInt64(db, count_sql, project_id, kEdgeCalls,
					  entity_id, &page.ok);
	return page;
}

// ── Findings ────────────────────────────────────────────────────

/// Findings whose evidence points at `path` (a file, or a module prefix).
///
/// `finding` has no location and no run: both live on evidence. The join is also
/// what keeps the count honest — a finding row is current state, while
/// membership comes from the evidence of the run that produced it (§6.1).
///
/// Boundary safety: `path` may be a file (`/proj/src/app.cpp`) or a module
/// (`/proj/src/app`). A bare `LIKE '%path%'` matches `/proj/src/app2/b.cpp`,
/// pulling in findings from a sibling directory. The SQL uses two conditions:
/// `file_path = path` (exact file) OR `file_path LIKE 'path/%'` (directory
/// prefix), preventing the `app` → `app2` cross-match.
Page<FindingRow> queryFindings(sqlite3 *db, uint64_t project_id,
			       const std::string &path)
{
	Page<FindingRow> page;
	// Two patterns: exact file match and directory-prefix match.
	const std::string dir_prefix = path + "/%";
	const char *sql =
		"SELECT f.finding_key, f.category, f.severity, f.description, "
		"       MIN(l.file_path), MIN(l.start_row) "
		"FROM finding f "
		"JOIN finding_evidence fe "
		"  ON fe.project_id = f.project_id "
		" AND fe.finding_key = f.finding_key "
		"JOIN evidence e ON e.id = fe.evidence_id "
		"JOIN evidence_location l ON l.evidence_id = e.id "
		"WHERE f.project_id = ? "
		"  AND (l.file_path = ? OR l.file_path LIKE ?) "
		"  AND f.status NOT IN ('resolved', 'superseded') "
		"GROUP BY f.finding_key "
		"ORDER BY f.severity DESC, f.finding_key LIMIT ?";
	const char *count_sql =
		"SELECT COUNT(DISTINCT f.finding_key) "
		"FROM finding f "
		"JOIN finding_evidence fe "
		"  ON fe.project_id = f.project_id "
		" AND fe.finding_key = f.finding_key "
		"JOIN evidence e ON e.id = fe.evidence_id "
		"JOIN evidence_location l ON l.evidence_id = e.id "
		"WHERE f.project_id = ? "
		"  AND (l.file_path = ? OR l.file_path LIKE ?) "
		"  AND f.status NOT IN ('resolved', 'superseded')";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, dir_prefix.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, kMaxFindings);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		FindingRow r;
		r.finding_key = textCol(stmt, 0);
		r.dimension = textCol(stmt, 1);
		r.severity = sqlite3_column_int64(stmt, 2);
		r.title = textCol(stmt, 3);
		r.file_path = textCol(stmt, 4);
		r.line = sqlite3_column_int64(stmt, 5);
		page.rows.push_back(std::move(r));
	}
	if (rc != SQLITE_DONE)
		page.ok = false;
	sqlite3_finalize(stmt);

	// Count with the same two-pattern predicate. Inline the
	// count so the bind positions match the SQL.
	sqlite3_stmt *counter = nullptr;
	if (sqlite3_prepare_v2(db, count_sql, -1, &counter, nullptr) !=
	    SQLITE_OK) {
		page.ok = false;
		return page;
	}
	sqlite3_bind_int64(counter, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(counter, 2, path.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(counter, 3, dir_prefix.c_str(), -1, SQLITE_TRANSIENT);
	if (sqlite3_step(counter) == SQLITE_ROW)
		page.total = sqlite3_column_int64(counter, 0);
	else
		page.ok = false;
	sqlite3_finalize(counter);
	return page;
}

// ── Test candidates ─────────────────────────────────────────────

/// Test files that mention the symbol. Naming-based, never execution: the
/// caller is told so in `note` and `unknowns` (development_plan §13.2 item 8).
Page<std::string> queryTestCandidates(sqlite3 *db, uint64_t project_id,
				      const std::string &symbol)
{
	Page<std::string> page;
	const char *sql = "SELECT DISTINCT e.file_path FROM entity e "
			  "WHERE e.project_id=? "
			  "AND (e.file_path LIKE '%test%' "
			  "     OR e.file_path LIKE '%Test%') "
			  "AND (e.name LIKE ? OR e.qualified_name LIKE ?) "
			  "ORDER BY e.file_path LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		page.ok = false;
		return page;
	}
	const std::string pattern = "%" + symbol + "%";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, kMaxTestCandidates);
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
		page.rows.push_back(textCol(stmt, 0));
	if (rc != SQLITE_DONE)
		page.ok = false;
	sqlite3_finalize(stmt);
	return page;
}

// ── Run info ────────────────────────────────────────────────────

RunInfo queryLatestRun(sqlite3 *db, uint64_t project_id)
{
	RunInfo info;
	const char *sql = "SELECT run_id, status FROM verification_run "
			  "WHERE project_id=? "
			  "ORDER BY started_at DESC, run_id DESC LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		info.ok = false;
		return info;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		info.run_id = textCol(stmt, 0);
		info.status = textCol(stmt, 1);
	}
	sqlite3_finalize(stmt);
	return info;
}

} // namespace engine_views
