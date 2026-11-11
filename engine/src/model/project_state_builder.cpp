// project_state_builder.cpp — Phase 4 Project State snapshot builder.
//
// Aggregates all per-project evidence + state rows into a single
// project_state row. The snapshot is a JSON object with one sub-object
// per category (sync / memory / error / pattern / framework / ffi)
// plus an overall confidence + inspector-count summary.
//
// Pipeline (plan §6.2):
//   1. evidence::EvidenceBuilder::buildAll → vector<Evidence>
//   2. Aggregate evidence by category: count items + collect titles.
//   3. SELECT COUNT(*) FROM semantic_fact WHERE category=? for a
//      per-category raw fact count.
//   4. SELECT counts from capability_state, architecture_state,
//      workflow_state, module_summary.dead_entities.
//   5. Compute overall confidence: 1.0 − penalty_per_issue × count,
//      clamped to [0.0, 1.0]. Penalty constants live in the header.
//   6. Build snapshot_json (see plan §6.2 schema).
//   7. UPSERT into project_state (ON CONFLICT project_id DO UPDATE).
//
// All sqlite3_stmt handles are managed by an RAII guard so they are
// finalized on every exit path. The store pointer is borrowed; null
// is checked at every public entry point and returns false / empty
// (no crash). Empty state tables are not errors — the snapshot simply
// omits the corresponding key (or sets its score to a default).

#include "project_state_builder.h"
#include "util/json_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../evidence/evidence_builder.h"
#include "../evidence/rule.h"
#include "../store/store.h"

namespace model
{

// ─── Local helpers ──────────────────────────────────────────────

namespace
{

// RAII guard around a sqlite3_stmt. Calls sqlite3_finalize on
// destruction unless release() is called (transfer ownership).
// Ensures statements are finalized on every exit path, including
// early returns on error. Mirrors the pattern used in
// evidence_builder.cpp.
class StmtGuard {
    public:
	explicit StmtGuard(sqlite3_stmt *stmt = nullptr)
		: stmt_(stmt)
	{
	}
	~StmtGuard()
	{
		if (stmt_)
			sqlite3_finalize(stmt_);
	}
	StmtGuard(const StmtGuard &) = delete;
	StmtGuard &operator=(const StmtGuard &) = delete;
	StmtGuard(StmtGuard &&other) noexcept
		: stmt_(other.stmt_)
	{
		other.stmt_ = nullptr;
	}
	sqlite3_stmt *get() const
	{
		return stmt_;
	}

    private:
	sqlite3_stmt *stmt_;
};

// Read a text column as a std::string; NULL → "".
std::string colText(sqlite3_stmt *stmt, int col)
{
	const unsigned char *t = sqlite3_column_text(stmt, col);
	if (!t)
		return "";
	return std::string(reinterpret_cast<const char *>(t));
}

// JSON-escape a string for inclusion in a JSON string literal.
// Mirrors the jsonEscape helper used across the engine.
std::string escapeJson(const std::string &s)
{
	return util::jsonEscapeString(s);
}

// Format a double as a JSON number with up to 4 decimal places.
// Trailing zeros are stripped (1.0000 → "1", 0.5000 → "0.5").
std::string fmtDouble(double v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.4f", v);
	std::string s(buf);
	// Strip trailing zeros and the dot if at the end.
	while (!s.empty() && s.back() == '0')
		s.pop_back();
	if (!s.empty() && s.back() == '.')
		s.pop_back();
	if (s.empty() || s == "-0")
		s = "0";
	return s;
}

// Return the current UTC timestamp in ISO 8601 format
// (YYYY-MM-DDTHH:MM:SSZ). Uses strftime for portability; the time
// zone is UTC because project_state snapshot consumers may run in
// any locale.
std::string currentUtcIsoTimestamp()
{
	std::time_t now = std::time(nullptr);
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buf;
}

// Per-category aggregated evidence: total item count across all
// rules in this category + a list of (title, item_count) for the
// snapshot details array.
struct CategoryAgg {
	int issue_count = 0;
	std::vector<std::string> details;
};

// Aggregate a vector of Evidence by category. Each Evidence contributes
// items.size() to its category's issue count. The first few titles are
// collected as detail strings (capped at kMaxDetailsPerCategory per
// category to keep the snapshot compact).
std::unordered_map<std::string, CategoryAgg>
aggregateByCategory(const std::vector<evidence::Evidence> &evidences)
{
	std::unordered_map<std::string, CategoryAgg> out;
	for (const auto &ev : evidences) {
		auto &agg = out[ev.category];
		agg.issue_count += static_cast<int>(ev.items.size());
		if (agg.details.size() < kMaxDetailsPerCategory) {
			std::string detail = ev.title;
			if (ev.items.size() > 1) {
				detail += " (" +
					  std::to_string(ev.items.size()) +
					  " items)";
			}
			agg.details.push_back(std::move(detail));
		}
	}
	return out;
}

// Serialize a vector of detail strings to a JSON array string.
// Empty vector returns "[]". Each string is JSON-escaped.
std::string serializeDetailsArray(const std::vector<std::string> &details)
{
	std::ostringstream ss;
	ss << "[";
	for (size_t i = 0; i < details.size(); ++i) {
		if (i)
			ss << ",";
		ss << "\"" << escapeJson(details[i]) << "\"";
	}
	ss << "]";
	return ss.str();
}

// Run a `SELECT COUNT(*) FROM <table> WHERE project_id = ?` query
// and return the count. Returns 0 on error (logged). Used to get
// per-category fact counts and state-row counts in one place.
int64_t countRows(store::GraphStore *store, const std::string &sql,
		  uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=countRows] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

// Distinct framework primitives detected in semantic_fact for the
// project under category='framework'. Returns a vector of unique
// primitive names (e.g. "gin", "gorm"). Empty on error.
std::vector<std::string> detectFrameworks(store::GraphStore *store,
					  uint64_t project_id)
{
	std::vector<std::string> out;
	if (!store || !store->handle())
		return out;
	const char *sql = "SELECT DISTINCT primitive FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'framework'";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=detectFrameworks] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store->handle()));
		return out;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		out.push_back(colText(stmt, 0));
	}
	return out;
}

// Query the count of findings for a project grouped by severity.
// Returns a map: {1: "info", 2: "warning", 3: "error"} → count.
// Used to populate the pattern.by_severity block in the snapshot.
std::map<int, int> findingSeverityCounts(store::GraphStore *store,
					 uint64_t project_id)
{
	std::map<int, int> out;
	if (!store || !store->handle())
		return out;
	const char *sql = "SELECT severity, COUNT(*) FROM finding "
			  "WHERE project_id = ? GROUP BY severity";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=findingSeverityCounts] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return out;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int sev = sqlite3_column_int(stmt, 0);
		int cnt = sqlite3_column_int(stmt, 1);
		out[sev] = cnt;
	}
	return out;
}

// Count verified capability_state rows (state='Implemented' or
// 'Verified'). Used for capability.verified in the snapshot.
int64_t countVerifiedCapabilities(store::GraphStore *store, uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	const char *sql = "SELECT COUNT(*) FROM capability_state "
			  "WHERE project_id = ? AND state IN "
			  "('Implemented','Verified')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=countVerifiedCapabilities] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

// Sum one architecture_state column for a project. `column` is chosen by
// the callers below (never client input) AND checked against an allowlist,
// because it is interpolated into the SQL — together the two keep this a
// constant query. Returns 0 on error or empty table.
static int64_t sumArchitectureColumn(store::GraphStore *store,
				     uint64_t project_id, const char *column)
{
	if (std::strcmp(column, "violations") != 0 &&
	    std::strcmp(column, "cross_module_edges") != 0) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumArchitectureColumn] "
			"refusing non-allowlisted column '%s'\n",
			column);
		return 0;
	}
	if (!store || !store->handle())
		return 0;
	const std::string sql = std::string("SELECT COALESCE(SUM(") + column +
				"), 0) FROM architecture_state "
				"WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumArchitectureColumn] "
			"prepare failed for column '%s': %s\n",
			column, sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

// Sum architecture_state.violations for a project. Returns 0 on error or
// empty table. Always 0 today: calling a cross-module dependency a violation
// needs a layer model, which this graph does not carry — see
// StateBuilder::buildArchitectureState.
int64_t sumArchitectureViolations(store::GraphStore *store, uint64_t project_id)
{
	return sumArchitectureColumn(store, project_id, "violations");
}

// Sum architecture_state.cross_module_edges: how many call edges cross a
// module boundary, summed over the tracked module pairs. This is a
// dependency count, NOT a violation count.
int64_t sumArchitectureCrossModuleEdges(store::GraphStore *store,
					uint64_t project_id)
{
	return sumArchitectureColumn(store, project_id, "cross_module_edges");
}

// Sum workflow_state.steps_done and steps_total across all workflows
// for a project. Returns (steps_done, steps_total). Used for the
// workflow score = done / total.
struct WorkflowProgress {
	int64_t steps_done = 0;
	int64_t steps_total = 0;
};
WorkflowProgress sumWorkflowProgress(store::GraphStore *store,
				     uint64_t project_id)
{
	WorkflowProgress wp;
	if (!store || !store->handle())
		return wp;
	const char *sql = "SELECT COALESCE(SUM(steps_done), 0), "
			  "       COALESCE(SUM(steps_total), 0) "
			  "FROM workflow_state WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumWorkflowProgress] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return wp;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		wp.steps_done = sqlite3_column_int64(stmt, 0);
		wp.steps_total = sqlite3_column_int64(stmt, 1);
	}
	return wp;
}

// Sum module_summary.dead_entities across all modules for a project.
// Used for the dead_code.entities count in the snapshot.
int64_t sumDeadEntities(store::GraphStore *store, uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	const char *sql = "SELECT COALESCE(SUM(dead_entities), 0) "
			  "FROM module_summary WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=sumDeadEntities] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store->handle()));
		return 0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	return count;
}

// Total entity count for a project (used to compute dead_code.pct =
// dead / total). Returns 0 on error or empty project.
int64_t countEntities(store::GraphStore *store, uint64_t project_id)
{
	return countRows(store,
			 "SELECT COUNT(*) FROM entity WHERE project_id = ?",
			 project_id);
}

// Compute the per-category block of the snapshot JSON. `agg` is the
// aggregated evidence for the category (may be absent if no rules
// fired). `fact_count` is the raw semantic_fact row count for the
// category. Returns the inner JSON object string (without enclosing
// braces); the caller wraps it in "category": {...}.
//
// Fields: "issues" (item count), "details" (array of titles).
std::string buildCategoryBlock(const CategoryAgg *agg, int64_t fact_count)
{
	std::ostringstream ss;
	int issue_count = agg ? agg->issue_count : 0;
	ss << "\"issues\":" << issue_count;
	ss << ",\"facts\":" << fact_count;
	ss << ",\"details\":";
	if (agg) {
		ss << serializeDetailsArray(agg->details);
	} else {
		ss << "[]";
	}
	return ss.str();
}

// Compute the overall confidence from the aggregated evidence and
// state counts. Starts at 1.0 and subtracts a per-issue penalty for
// each category, then clamps to [0.0, 1.0]. See penalty constants
// in project_state_builder.h.
double computeOverallConfidence(
	const std::unordered_map<std::string, CategoryAgg> &agg,
	int64_t arch_violations, int64_t dead_entities)
{
	double confidence = 1.0;
	auto subtract = [&](const std::string &cat, double penalty) {
		auto it = agg.find(cat);
		if (it == agg.end())
			return;
		confidence -=
			penalty * static_cast<double>(it->second.issue_count);
	};
	subtract("sync", kPenaltyPerSyncIssue);
	subtract("memory", kPenaltyPerMemoryIssue);
	subtract("error", kPenaltyPerErrorIssue);
	subtract("pattern", kPenaltyPerPatternIssue);
	subtract("ffi", kPenaltyPerFfiIssue);
	confidence -= kPenaltyPerArchitectureViolation *
		      static_cast<double>(arch_violations);
	confidence -=
		kPenaltyPerDeadEntity * static_cast<double>(dead_entities);
	if (confidence < 0.0)
		confidence = 0.0;
	if (confidence > 1.0)
		confidence = 1.0;
	return confidence;
}

// Count how many distinct inspector categories produced evidence.
// Used for the "overall.inspectors_ran" field. The maximum is the
// number of categories with rule files (sync/memory/error/pattern/
// framework/ffi = 6).
int countInspectorsRan(const std::unordered_map<std::string, CategoryAgg> &agg)
{
	return static_cast<int>(agg.size());
}

// Persist the snapshot + confidence to project_state via UPSERT.
// Returns true on success, false on prepare/step failure (logged).
bool upsertProjectState(store::GraphStore *store, uint64_t project_id,
			double confidence, const std::string &snapshot_json)
{
	if (!store || !store->handle())
		return false;
	const char *sql = "INSERT INTO project_state "
			  "(project_id, confidence, snapshot_json, updated_at) "
			  "VALUES (?,?,?,datetime('now')) "
			  "ON CONFLICT(project_id) DO UPDATE SET "
			  "confidence=excluded.confidence, "
			  "snapshot_json=excluded.snapshot_json, "
			  "updated_at=datetime('now')";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=upsertProjectState] "
			"prepare failed: %s\n",
			sqlite3_errmsg(store->handle()));
		return false;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_double(stmt, 2, confidence);
	sqlite3_bind_text(stmt, 3, snapshot_json.c_str(), -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		std::fprintf(
			stderr,
			"[module=project_state, method=upsertProjectState] "
			"step failed (rc=%d): %s\n",
			rc, sqlite3_errmsg(store->handle()));
		return false;
	}
	return true;
}

} // namespace

// ─── ProjectStateBuilder public API ──────────────────────────────

/// Render one category block. Every call site used to inline the same
/// find/end/ternary dance; collapsing it here is what lets build() shrink.
std::string
categoryBlockFor(const std::unordered_map<std::string, CategoryAgg> &agg,
		 const char *key, int64_t fact_count)
{
	auto it = agg.find(key);
	return buildCategoryBlock(it != agg.end() ? &it->second : nullptr,
				  fact_count);
}

/// Score values as they appear in the snapshot JSON.
///
/// Grouped by block rather than kept flat because a single struct holding all
/// of them would exceed the 12-field limit of code_rules §1.4 — and because
/// each group is what one JSON block actually reads.
struct SnapshotScores {
	double confidence = 0.0;
	double arch_score = 0.0;
	double capability_score = 0.0;
	double workflow_score = 0.0;
	double dead_pct = 0.0;
	int inspectors_ran = 0;
	int64_t capability_total = 0;
	int64_t capability_verified = 0;
	int64_t arch_violations = 0;
	int64_t arch_cross_module = 0;
};

/// Denominators behind the scores: shown in the snapshot so a reader can tell
/// "0 of 0" from "0 of 100".
struct SnapshotScale {
	int64_t workflow_total = 0;
	int64_t steps_done = 0;
	int64_t steps_total = 0;
	int64_t dead_entities = 0;
	int64_t total_entities = 0;
};

/// Fact counts per reported category.
struct CategoryTotals {
	int64_t sync = 0;
	int64_t memory = 0;
	int64_t error = 0;
	int64_t pattern = 0;
	int64_t ffi = 0;
	int64_t framework = 0;
};

/// Severity split of the pattern findings.
struct SeverityCounts {
	int info = 0;
	int warning = 0;
	int error = 0;
};

/// Write the snapshot JSON. Split out of build() so that function keeps its
/// grandfathered size (code_rules §1.1: over-limit code may shrink, not grow).
void writeSnapshotJson(std::ostringstream &ss, const SnapshotScores &scores,
		       const SnapshotScale &scale, const CategoryTotals &totals,
		       const SeverityCounts &severity,
		       const std::unordered_map<std::string, CategoryAgg> &agg,
		       const std::vector<std::string> &frameworks)
{
	ss << "{\"score_basis\":\"legacy_heuristic\",";
	ss << "\"overall\":{\"confidence\":" << fmtDouble(scores.confidence)
	   << ",\"inspectors_ran\":" << scores.inspectors_ran << "}";

	ss << ",\"capability\":{\"score\":"
	   << fmtDouble(scores.capability_score)
	   << ",\"total\":" << scores.capability_total
	   << ",\"verified\":" << scores.capability_verified << "}";

	ss << ",\"architecture\":{\"score\":" << fmtDouble(scores.arch_score)
	   << ",\"violations\":" << scores.arch_violations
	   << ",\"cross_module_edges\":" << scores.arch_cross_module << "}";

	ss << ",\"workflow\":{\"score\":" << fmtDouble(scores.workflow_score)
	   << ",\"total\":" << scale.workflow_total
	   << ",\"steps_done\":" << scale.steps_done
	   << ",\"steps_total\":" << scale.steps_total << "}";

	ss << ",\"dead_code\":{\"pct\":" << fmtDouble(scores.dead_pct)
	   << ",\"entities\":" << scale.dead_entities
	   << ",\"total\":" << scale.total_entities << "}";

	ss << ",\"sync\":{" << categoryBlockFor(agg, "sync", totals.sync)
	   << "}";
	ss << ",\"memory\":{" << categoryBlockFor(agg, "memory", totals.memory)
	   << "}";
	ss << ",\"error_handling\":{"
	   << categoryBlockFor(agg, "error", totals.error) << "}";
	ss << ",\"pattern\":{"
	   << categoryBlockFor(agg, "pattern", totals.pattern)
	   << ",\"by_severity\":{\"info\":" << severity.info
	   << ",\"warning\":" << severity.warning
	   << ",\"error\":" << severity.error << "}}";

	ss << ",\"framework\":{\"detected\":[";
	for (size_t i = 0; i < frameworks.size(); ++i) {
		if (i)
			ss << ",";
		ss << "\"" << escapeJson(frameworks[i]) << "\"";
	}
	ss << "],\"facts\":" << totals.framework << "}";

	ss << ",\"ffi\":{" << categoryBlockFor(agg, "ffi", totals.ffi)
	   << ",\"boundaries\":" << totals.ffi << "}";

	ss << ",\"last_updated\":\"" << currentUtcIsoTimestamp() << "\"";
	ss << "}";
}

double clamp01(double v)
{
	if (v < 0.0)
		return 0.0;
	if (v > 1.0)
		return 1.0;
	return v;
}

/// Raw fact counts per category, straight from semantic_fact.
///
/// One helper instead of six copies of the same query: the six differ only in
/// the category literal, and a typo in one of them used to be invisible.
CategoryTotals countCategoryTotals(store::GraphStore *store,
				   uint64_t project_id)
{
	CategoryTotals t;
	t.sync = countRows(
		store,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id = ? "
		"AND category = 'sync'",
		project_id);
	t.memory = countRows(
		store,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id = ? "
		"AND category = 'memory'",
		project_id);
	t.error = countRows(
		store,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id = ? "
		"AND category = 'error'",
		project_id);
	t.pattern = countRows(
		store,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id = ? "
		"AND category = 'pattern'",
		project_id);
	t.framework = countRows(
		store,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id = ? "
		"AND category = 'framework'",
		project_id);
	t.ffi = countRows(
		store,
		"SELECT COUNT(*) FROM semantic_fact WHERE project_id = ? "
		"AND category = 'ffi'",
		project_id);
	return t;
}

/// Severity split of the stored findings.
SeverityCounts countSeverities(store::GraphStore *store, uint64_t project_id)
{
	SeverityCounts out;
	for (const auto &kv : findingSeverityCounts(store, project_id)) {
		switch (kv.first) {
		case 1:
			out.warning += kv.second;
			break;
		case 2:
			out.error += kv.second;
			break;
		default:
			// 0 is info; an unknown severity is treated as info rather
			// than dropped, so the buckets always sum to the total.
			out.info += kv.second;
			break;
		}
	}
	return out;
}

/// Turn the raw counts into the scores the snapshot reports.
///
/// Kept separate from build() so the clamping rules live in one place: every
/// score is 0..1, and "no data" is 1.0 only for capability/workflow (nothing
/// to fail) and 0.0 for dead code (nothing to be dead).
SnapshotScores
computeScores(const std::unordered_map<std::string, CategoryAgg> &agg,
	      const SnapshotScale &scale, int64_t capability_total,
	      int64_t capability_verified, int64_t arch_violations,
	      int64_t arch_cross_module)
{
	SnapshotScores s;
	s.capability_total = capability_total;
	s.capability_verified = capability_verified;
	s.arch_violations = arch_violations;
	s.arch_cross_module = arch_cross_module;
	s.confidence = computeOverallConfidence(agg, arch_violations,
						scale.dead_entities);
	// `violations` counts layer violations only; cross-module edges are
	// reported next to the score, not deducted from it.
	s.arch_score =
		clamp01(1.0 - kPenaltyPerArchitectureViolation *
				      static_cast<double>(arch_violations));
	s.capability_score =
		capability_total > 0 ?
			clamp01(static_cast<double>(capability_verified) /
				static_cast<double>(capability_total)) :
			1.0;
	s.workflow_score =
		scale.steps_total > 0 ?
			clamp01(static_cast<double>(scale.steps_done) /
				static_cast<double>(scale.steps_total)) :
			1.0;
	s.dead_pct =
		scale.total_entities > 0 ?
			clamp01(static_cast<double>(scale.dead_entities) /
				static_cast<double>(scale.total_entities)) :
			0.0;
	return s;
}

bool ProjectStateBuilder::build(uint64_t project_id)
{
	if (!store_) {
		std::fprintf(stderr, "[module=project_state, method=build] "
				     "store is null\n");
		return false;
	}

	// 1. Run evidence::EvidenceBuilder::buildAll to get all evidence.
	evidence::EvidenceBuilder ev_builder(store_);
	ev_builder.loadRules(evidence::resolveRulesDir());
	auto evidences = ev_builder.buildAll(project_id);

	// 2. Aggregate evidence by category (count + titles).
	auto agg = aggregateByCategory(evidences);

	// 3-5. Gather the numbers the snapshot reports, then score them.
	CategoryTotals totals = countCategoryTotals(store_, project_id);
	int64_t capability_total =
		countRows(store_,
			  "SELECT COUNT(*) FROM capability_state "
			  "WHERE project_id = ?",
			  project_id);
	int64_t capability_verified =
		countVerifiedCapabilities(store_, project_id);
	int64_t arch_violations = sumArchitectureViolations(store_, project_id);
	int64_t arch_cross_module =
		sumArchitectureCrossModuleEdges(store_, project_id);
	WorkflowProgress wp = sumWorkflowProgress(store_, project_id);
	int64_t workflow_total =
		countRows(store_,
			  "SELECT COUNT(*) FROM workflow_state "
			  "WHERE project_id = ?",
			  project_id);
	SnapshotScale scale{ workflow_total, wp.steps_done, wp.steps_total,
			     sumDeadEntities(store_, project_id),
			     countEntities(store_, project_id) };
	SnapshotScores scores =
		computeScores(agg, scale, capability_total, capability_verified,
			      arch_violations, arch_cross_module);
	scores.inspectors_ran = countInspectorsRan(agg);

	// Detect frameworks (distinct primitives in semantic_fact).
	auto frameworks = detectFrameworks(store_, project_id);
	SeverityCounts severity = countSeverities(store_, project_id);

	// 6. Build snapshot_json. The structure follows plan §6.2.
	std::ostringstream ss;
	writeSnapshotJson(ss, scores, scale, totals, severity, agg, frameworks);
	std::string snapshot_json = ss.str();

	// 7. UPSERT into project_state.
	if (!upsertProjectState(store_, project_id, scores.confidence,
				snapshot_json)) {
		return false;
	}

	std::fprintf(stderr,
		     "[model] ProjectState: project_id=%llu "
		     "confidence=%.4f snapshot_bytes=%zu\n",
		     static_cast<unsigned long long>(project_id),
		     scores.confidence, snapshot_json.size());
	return true;
}

std::string ProjectStateBuilder::getSnapshotJson(uint64_t project_id) const
{
	if (!store_ || !store_->handle())
		return "";
	const char *sql = "SELECT snapshot_json FROM project_state "
			  "WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=getSnapshotJson] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store_->handle()));
		return "";
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string out;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		out = colText(stmt, 0);
	}
	return out;
}

double ProjectStateBuilder::getConfidence(uint64_t project_id) const
{
	if (!store_ || !store_->handle())
		return 0.0;
	const char *sql = "SELECT confidence FROM project_state "
			  "WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=project_state, method=getConfidence] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(store_->handle()));
		return 0.0;
	}
	StmtGuard guard(stmt);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	double out = 0.0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		out = sqlite3_column_double(stmt, 0);
	}
	return out;
}

} // namespace model
