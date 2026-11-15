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

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <memory>
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

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

/// Keep the SQLite cause in logs while callers surface an unknown result.
bool QueryFailed(sqlite3 *db, const char *operation, uint64_t project_id)
{
	std::fprintf(stderr,
		     "[module=analyze_queries, method=%s, project=%" PRIu64
		     "] %s\n",
		     operation, project_id, sqlite3_errmsg(db));
	return false;
}

// Only a measured C/C++ signature may pair rows. Kind 0/1 is one callable
// family because the visitor emits out-of-class methods as Functions.
// Language matching uses C-family equivalence so .h headers (detected as
// 'cpp' by filter_policy) pair with .c definitions (detected as 'c').
constexpr const char *kSameCallable =
	"e2.project_id=entity.project_id "
	"AND entity.kind IN (0,1) AND e2.kind IN (0,1) "
	"AND entity.language IN ('c','cpp','c++') "
	"AND e2.language IN ('c','cpp','c++') "
	"AND e2.name=entity.name "
	"AND COALESCE(NULLIF(e2.qualified_name,''),e2.name)="
	"COALESCE(NULLIF(entity.qualified_name,''),entity.name) "
	"AND entity.callable_signature!='' "
	"AND e2.callable_signature=entity.callable_signature ";

/// The relation scope includes the displayed entity plus its paired source
/// rows. Reused by callers, callees and call-edge test candidates; not capped.
std::string SymbolEntitiesSql()
{
	return std::string(
		       "WITH symbol_entities AS (SELECT e2.id FROM entity e2 "
		       "JOIN entity ON entity.id=?3 AND entity.project_id=?1 "
		       "WHERE (e2.id=entity.id OR (") +
	       kSameCallable + ")) AND (?4='' OR e2.file_path LIKE ?4)) ";
}

/// Bind the same lookup scope to row and count queries. Empty path means all.
bool BindSymbolScope(sqlite3_stmt *stmt, uint64_t project_id, int type,
		     int64_t entity_id, const std::string &file_filter)
{
	const std::string pattern =
		file_filter.empty() ? "" : "%" + file_filter + "%";
	return sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id)) ==
		       SQLITE_OK &&
	       sqlite3_bind_int(stmt, 2, type) == SQLITE_OK &&
	       sqlite3_bind_int64(stmt, 3, entity_id) == SQLITE_OK &&
	       sqlite3_bind_text(stmt, 4, pattern.c_str(), -1,
				 SQLITE_TRANSIENT) == SQLITE_OK;
}

// ── Small SQL helpers ───────────────────────────────────────────

std::string textCol(sqlite3_stmt *stmt, int col)
{
	const char *text =
		reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
	return text ? text : "";
}

/// Count the uncapped relation set with exactly the same symbol/path bindings
/// as the row query. Failure stays unknown instead of becoming a zero total.
int64_t CountSymbolRelations(sqlite3 *db, const std::string &sql,
			     uint64_t project_id, int64_t entity_id,
			     const std::string &file_filter, bool *ok)
{
	sqlite3_stmt *stmt = nullptr;
	const int prepared =
		sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
	Statement owned(stmt, sqlite3_finalize);
	if (prepared != SQLITE_OK ||
	    !BindSymbolScope(stmt, project_id, kEdgeCalls, entity_id,
			     file_filter)) {
		*ok = QueryFailed(db, "CountSymbolRelations.prepare",
				  project_id);
		return -1;
	}
	int64_t total = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		total = sqlite3_column_int64(stmt, 0);
	else
		*ok = QueryFailed(db, "CountSymbolRelations.step", project_id);
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
		"WHERE project_id=?1 AND (name=?2 OR qualified_name=?2)";
	if (has_filter)
		where += " AND file_path LIKE ?3";
	// Prefer only a signature-matched definition inside the requested scope.
	// Unknown legacy signatures keep their candidates, never guess a pair.
	where += " AND (is_definition=1 OR NOT EXISTS ("
		 "SELECT 1 FROM entity e2 WHERE ";
	where += kSameCallable;
	where += "AND e2.is_definition=1";
	if (has_filter)
		where += " AND e2.file_path LIKE ?3";
	where += "))";

	std::string select =
		"SELECT id, name, qualified_name, kind, file_path, "
		"       start_row, language, module_path FROM entity " +
		where + " ORDER BY id LIMIT " +
		std::to_string(kMaxResolvedEntities);
	std::string count = "SELECT COUNT(*) FROM entity " + where;

	const std::string pattern = "%" + file_filter + "%";
	const auto bind_lookup = [&](sqlite3_stmt *stmt) {
		return sqlite3_bind_int64(stmt, 1,
					  static_cast<int64_t>(project_id)) ==
			       SQLITE_OK &&
		       sqlite3_bind_text(stmt, 2, name.c_str(), -1,
					 SQLITE_TRANSIENT) == SQLITE_OK &&
		       (!has_filter ||
			sqlite3_bind_text(stmt, 3, pattern.c_str(), -1,
					  SQLITE_TRANSIENT) == SQLITE_OK);
	};
	sqlite3_stmt *stmt = nullptr;
	const int prepared =
		sqlite3_prepare_v2(db, select.c_str(), -1, &stmt, nullptr);
	Statement owned(stmt, sqlite3_finalize);
	if (prepared != SQLITE_OK || !bind_lookup(stmt)) {
		page.ok =
			QueryFailed(db, "resolveEntities.prepare", project_id);
		return page;
	}
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
		page.rows.push_back(readEntityRow(stmt));
	if (rc != SQLITE_DONE)
		page.ok = QueryFailed(db, "resolveEntities.step", project_id);
	owned.reset();

	// The count repeats the predicate; it is not derived from the page.
	sqlite3_stmt *counter = nullptr;
	const int counted =
		sqlite3_prepare_v2(db, count.c_str(), -1, &counter, nullptr);
	Statement owned_counter(counter, sqlite3_finalize);
	if (counted != SQLITE_OK || !bind_lookup(counter)) {
		page.ok = QueryFailed(db, "resolveEntities.count", project_id);
		return page;
	}
	if (sqlite3_step(counter) == SQLITE_ROW)
		page.total = sqlite3_column_int64(counter, 0);
	else
		page.ok = QueryFailed(db, "resolveEntities.count", project_id);
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
				 int64_t entity_id, bool outgoing,
				 const std::string &file_filter)
{
	Page<RelationRow> page;
	const std::string join =
		outgoing ?
			"FROM relation r JOIN entity e ON e.id=r.target_id " :
			"FROM relation r JOIN entity e ON e.id=r.source_id ";
	const std::string where =
		outgoing ?
			"WHERE r.project_id=?1 AND r.type=?2 AND r.source_id "
			"IN (SELECT id FROM symbol_entities) " :
			"WHERE r.project_id=?1 AND r.type=?2 AND r.target_id "
			"IN (SELECT id FROM symbol_entities) ";
	const std::string scope_sql = SymbolEntitiesSql();
	const std::string sql =
		scope_sql +
		"SELECT COALESCE(NULLIF(e.qualified_name,''),e.name), "
		"e.file_path,e.start_row,r.type " +
		join + where + "GROUP BY e.id ORDER BY e.id LIMIT ?5";
	const std::string count_sql =
		scope_sql + "SELECT COUNT(DISTINCT e.id) " + join + where;

	sqlite3_stmt *stmt = nullptr;
	const int prepared =
		sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
	Statement owned(stmt, sqlite3_finalize);
	if (prepared != SQLITE_OK ||
	    !BindSymbolScope(stmt, project_id, kEdgeCalls, entity_id,
			     file_filter) ||
	    sqlite3_bind_int(stmt, 5, outgoing ? kMaxCallees : kMaxCallers) !=
		    SQLITE_OK) {
		page.ok = QueryFailed(db, "queryRelations.prepare", project_id);
		return page;
	}
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
		page.ok = QueryFailed(db, "queryRelations.step", project_id);
	owned.reset();

	page.total = CountSymbolRelations(db, count_sql, project_id, entity_id,
					  file_filter, &page.ok);
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
Page<TestCandidate> queryTestCandidates(sqlite3 *db, uint64_t project_id,
					int64_t entity_id,
					const std::string &symbol,
					const std::string &file_filter)
{
	Page<TestCandidate> page;
	if (symbol.empty() && entity_id <= 0)
		return page;

	// Pass 1: a test entity calls the symbol. This is evidence, not a guess.
	const std::string call_sql =
		SymbolEntitiesSql() +
		"SELECT DISTINCT e.file_path, "
		"       COALESCE(NULLIF(e.qualified_name, ''), e.name), "
		"       e.start_row "
		"FROM relation r "
		"JOIN entity e ON e.id = r.source_id "
		"JOIN files f ON f.project_id = e.project_id "
		"            AND f.path = e.file_path "
		"WHERE r.project_id = ?1 AND r.type = ?2 AND r.target_id "
		"IN (SELECT id FROM symbol_entities) "
		"  AND f.is_test = 1 "
		"ORDER BY e.file_path, e.start_row LIMIT ?5";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, call_sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		page.ok = QueryFailed(db, "queryTestCandidates.prepare",
				      project_id);
		return page;
	}
	if (!BindSymbolScope(stmt, project_id, kEdgeCalls, entity_id,
			     file_filter) ||
	    sqlite3_bind_int(stmt, 5, kMaxTestCandidates) != SQLITE_OK) {
		page.ok =
			QueryFailed(db, "queryTestCandidates.bind", project_id);
		sqlite3_finalize(stmt);
		return page;
	}
	int rc = SQLITE_ROW;
	std::vector<std::string> seen;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		TestCandidate c;
		c.file_path = textCol(stmt, 0);
		c.entity = textCol(stmt, 1);
		c.line = sqlite3_column_int64(stmt, 2);
		c.method = "call_edge";
		c.confidence = "high";
		seen.push_back(c.file_path);
		page.rows.push_back(std::move(c));
	}
	if (rc != SQLITE_DONE)
		page.ok =
			QueryFailed(db, "queryTestCandidates.step", project_id);
	sqlite3_finalize(stmt);
	if (!page.ok)
		return page;

	// Pass 2: a test file mentions the name but nothing calls the symbol.
	// Lower confidence, and never used to contradict pass 1.
	const char *name_sql =
		"SELECT DISTINCT e.file_path, "
		"       COALESCE(NULLIF(e.qualified_name, ''), e.name), "
		"       e.start_row "
		"FROM entity e "
		"JOIN files f ON f.project_id = e.project_id "
		"            AND f.path = e.file_path "
		"WHERE e.project_id = ? AND f.is_test = 1 "
		"  AND (e.name LIKE ? OR e.qualified_name LIKE ?) "
		"ORDER BY e.file_path, e.start_row LIMIT ?";
	if (sqlite3_prepare_v2(db, name_sql, -1, &stmt, nullptr) != SQLITE_OK) {
		page.ok = QueryFailed(db, "queryTestCandidates.p2.prepare",
				      project_id);
		return page;
	}
	const std::string pattern = "%" + symbol + "%";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, kMaxTestCandidates);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		TestCandidate c;
		c.file_path = textCol(stmt, 0);
		if (std::find(seen.begin(), seen.end(), c.file_path) !=
		    seen.end())
			continue;
		c.entity = textCol(stmt, 1);
		c.line = sqlite3_column_int64(stmt, 2);
		c.method = "name_match";
		c.confidence = "low";
		seen.push_back(c.file_path);
		page.rows.push_back(std::move(c));
	}
	if (rc != SQLITE_DONE)
		page.ok = QueryFailed(db, "queryTestCandidates.p2.step",
				      project_id);
	sqlite3_finalize(stmt);
	page.total = static_cast<int64_t>(seen.size());
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
