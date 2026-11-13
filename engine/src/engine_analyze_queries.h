#ifndef CODESCOPE_ENGINE_ANALYZE_QUERIES_H
#define CODESCOPE_ENGINE_ANALYZE_QUERIES_H

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

/// SQL reads behind analyze_project's task-context views.
///
/// Separated from the sections that render them: the queries decide what a
/// failed read looks like (unknown, never an empty list) and what a real total
/// is, while the writers only format.
namespace engine_views
{

// ── Display caps (§18.7.0) ─────────────────────────────────────
constexpr int kMaxCallers = 30;
constexpr int kMaxCallees = 30;
constexpr int kMaxFindings = 20;
constexpr int kMaxTestCandidates = 15;
constexpr int kMaxModuleEntities = 50;
constexpr int kMaxResolvedEntities = 50;

struct EntityRow {
	int64_t id = 0;
	std::string name;
	std::string qualified_name;
	int kind = 0;
	std::string file_path;
	int64_t start_row = 0;
	std::string language;
	std::string module_path;
};

struct RelationRow {
	std::string entity_name;
	std::string file_path;
	int64_t line = 0;
	int edge_type = 0;
};

struct FindingRow {
	std::string finding_key;
	std::string dimension;
	int64_t severity = 0;
	std::string title;
	std::string file_path;
	int64_t line = 0;
};

/// A page of rows plus the exact total behind it.
///
/// Counting separately is what lets `total` stay true when the page is capped;
/// copying `rows.size()` reported "25 → 20" as the total.
template <typename T> struct Page {
	std::vector<T> rows;
	int64_t total = 0;
	/// False when the query itself failed: the caller reports unknown, not
	/// "zero results".
	bool ok = true;
};

struct RunInfo {
	std::string run_id;
	std::string status;
	bool ok = true;
};

/// Entities matching a bare or qualified name, optionally filtered by path.
Page<EntityRow> resolveEntities(sqlite3 *db, uint64_t project_id,
				const std::string &name,
				const std::string &file_filter);

/// Entities in a module (module_path prefix, already normalized).
Page<EntityRow> queryModuleEntities(sqlite3 *db, uint64_t project_id,
				    const std::string &module_path);

/// Callers (`outgoing == false`) or callees of one entity. Only
/// `EdgeType::Calls` is matched: `Contains` is ownership, not a call.
Page<RelationRow> queryRelations(sqlite3 *db, uint64_t project_id,
				 int64_t entity_id, bool outgoing);

/// Findings whose evidence points at `path` (file or module prefix).
Page<FindingRow> queryFindings(sqlite3 *db, uint64_t project_id,
			       const std::string &path);

/// Test files mentioning the symbol. Naming-based, never execution.
Page<std::string> queryTestCandidates(sqlite3 *db, uint64_t project_id,
				      const std::string &symbol);

/// Most recent verification run, whatever its status.
RunInfo queryLatestRun(sqlite3 *db, uint64_t project_id);

} // namespace engine_views

#endif // CODESCOPE_ENGINE_ANALYZE_QUERIES_H
