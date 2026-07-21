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
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x",
					      static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out;
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

// Sum architecture_state.violations for a project. Returns 0 on
// error or empty table.
int64_t sumArchitectureViolations(store::GraphStore *store, uint64_t project_id)
{
	if (!store || !store->handle())
		return 0;
	const char *sql = "SELECT COALESCE(SUM(violations), 0) "
			  "FROM architecture_state WHERE project_id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(
			stderr,
			"[module=project_state, method=sumArchitectureViolations] "
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

// Resolve the rules directory from CODESCOPE_RULES_DIR env var, or
// fall back to the default relative path "engine/src/evidence/rules".
// Mirrors the fallback used in engine_evidence_ffi.cpp.
std::string resolveRulesDir()
{
	const char *env_dir = std::getenv("CODESCOPE_RULES_DIR");
	if (env_dir && *env_dir)
		return std::string(env_dir);
	return "engine/src/evidence/rules";
}

} // namespace

// ─── ProjectStateBuilder public API ──────────────────────────────

bool ProjectStateBuilder::build(uint64_t project_id)
{
	if (!store_) {
		std::fprintf(stderr, "[module=project_state, method=build] "
				     "store is null\n");
		return false;
	}

	// 1. Run evidence::EvidenceBuilder::buildAll to get all evidence.
	evidence::EvidenceBuilder ev_builder(store_);
	ev_builder.loadRules(resolveRulesDir());
	auto evidences = ev_builder.buildAll(project_id);

	// 2. Aggregate evidence by category (count + titles).
	auto agg = aggregateByCategory(evidences);

	// 3. Query semantic_fact for per-category raw fact counts.
	int64_t sync_facts =
		countRows(store_,
			  "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'sync'",
			  project_id);
	int64_t memory_facts =
		countRows(store_,
			  "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'memory'",
			  project_id);
	int64_t error_facts =
		countRows(store_,
			  "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'error'",
			  project_id);
	int64_t pattern_facts =
		countRows(store_,
			  "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'pattern'",
			  project_id);
	int64_t framework_facts =
		countRows(store_,
			  "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'framework'",
			  project_id);
	int64_t ffi_facts =
		countRows(store_,
			  "SELECT COUNT(*) FROM semantic_fact "
			  "WHERE project_id = ? AND category = 'ffi'",
			  project_id);

	// 4. Query state tables for capability / architecture / workflow /
	//    dead_code counts.
	int64_t capability_total =
		countRows(store_,
			  "SELECT COUNT(*) FROM capability_state "
			  "WHERE project_id = ?",
			  project_id);
	int64_t capability_verified =
		countVerifiedCapabilities(store_, project_id);
	int64_t arch_violations = sumArchitectureViolations(store_, project_id);
	int64_t workflow_total =
		countRows(store_,
			  "SELECT COUNT(*) FROM workflow_state "
			  "WHERE project_id = ?",
			  project_id);
	WorkflowProgress wp = sumWorkflowProgress(store_, project_id);
	int64_t dead_entities = sumDeadEntities(store_, project_id);
	int64_t total_entities = countEntities(store_, project_id);

	// 5. Compute overall confidence (1.0 - sum of penalties).
	double confidence =
		computeOverallConfidence(agg, arch_violations, dead_entities);

	// Architecture score = 1 - violations * penalty (clamped).
	double arch_score = 1.0 - kPenaltyPerArchitectureViolation *
					  static_cast<double>(arch_violations);
	if (arch_score < 0.0)
		arch_score = 0.0;
	if (arch_score > 1.0)
		arch_score = 1.0;

	// Capability score = verified / total (or 1.0 when no
	// capabilities are tracked).
	double capability_score = 1.0;
	if (capability_total > 0) {
		capability_score = static_cast<double>(capability_verified) /
				   static_cast<double>(capability_total);
		if (capability_score < 0.0)
			capability_score = 0.0;
		if (capability_score > 1.0)
			capability_score = 1.0;
	}

	// Workflow score = done / total (or 1.0 when no workflows).
	double workflow_score = 1.0;
	if (wp.steps_total > 0) {
		workflow_score = static_cast<double>(wp.steps_done) /
				 static_cast<double>(wp.steps_total);
		if (workflow_score < 0.0)
			workflow_score = 0.0;
		if (workflow_score > 1.0)
			workflow_score = 1.0;
	}

	// Dead code percentage = dead / total (0.0 when no entities).
	double dead_pct = 0.0;
	if (total_entities > 0) {
		dead_pct = static_cast<double>(dead_entities) /
			   static_cast<double>(total_entities);
		if (dead_pct < 0.0)
			dead_pct = 0.0;
		if (dead_pct > 1.0)
			dead_pct = 1.0;
	}

	// Detect frameworks (distinct primitives in semantic_fact).
	auto frameworks = detectFrameworks(store_, project_id);

	// Finding severity counts (for pattern.by_severity block).
	auto findings = findingSeverityCounts(store_, project_id);
	int info_count = 0, warning_count = 0, error_count = 0;
	for (const auto &kv : findings) {
		switch (kv.first) {
		case 0:
			info_count += kv.second;
			break;
		case 1:
			warning_count += kv.second;
			break;
		case 2:
			error_count += kv.second;
			break;
		default:
			// Unknown severity → treat as info.
			info_count += kv.second;
			break;
		}
	}

	int inspectors_ran = countInspectorsRan(agg);

	// 6. Build snapshot_json. The structure follows plan §6.2.
	std::ostringstream ss;
	ss << "{";
	// overall
	ss << "\"overall\":{\"confidence\":" << fmtDouble(confidence)
	   << ",\"inspectors_ran\":" << inspectors_ran << "}";

	// capability
	ss << ",\"capability\":{\"score\":" << fmtDouble(capability_score)
	   << ",\"total\":" << capability_total
	   << ",\"verified\":" << capability_verified << "}";

	// architecture
	ss << ",\"architecture\":{\"score\":" << fmtDouble(arch_score)
	   << ",\"violations\":" << arch_violations << "}";

	// workflow
	ss << ",\"workflow\":{\"score\":" << fmtDouble(workflow_score)
	   << ",\"total\":" << workflow_total
	   << ",\"steps_done\":" << wp.steps_done
	   << ",\"steps_total\":" << wp.steps_total << "}";

	// dead_code
	ss << ",\"dead_code\":{\"pct\":" << fmtDouble(dead_pct)
	   << ",\"entities\":" << dead_entities
	   << ",\"total\":" << total_entities << "}";

	// sync
	auto sync_it = agg.find("sync");
	ss << ",\"sync\":{"
	   << buildCategoryBlock(sync_it != agg.end() ? &sync_it->second :
							nullptr,
				 sync_facts)
	   << "}";

	// memory
	auto memory_it = agg.find("memory");
	ss << ",\"memory\":{"
	   << buildCategoryBlock(memory_it != agg.end() ? &memory_it->second :
							  nullptr,
				 memory_facts)
	   << "}";

	// error_handling
	auto error_it = agg.find("error");
	ss << ",\"error_handling\":{"
	   << buildCategoryBlock(error_it != agg.end() ? &error_it->second :
							 nullptr,
				 error_facts)
	   << "}";

	// pattern
	auto pattern_it = agg.find("pattern");
	ss << ",\"pattern\":{"
	   << buildCategoryBlock(pattern_it != agg.end() ? &pattern_it->second :
							   nullptr,
				 pattern_facts)
	   << ",\"by_severity\":{\"info\":" << info_count
	   << ",\"warning\":" << warning_count << ",\"error\":" << error_count
	   << "}}";

	// framework
	ss << ",\"framework\":{\"detected\":[";
	for (size_t i = 0; i < frameworks.size(); ++i) {
		if (i)
			ss << ",";
		ss << "\"" << escapeJson(frameworks[i]) << "\"";
	}
	ss << "],\"facts\":" << framework_facts << "}";

	// ffi
	auto ffi_it = agg.find("ffi");
	ss << ",\"ffi\":{"
	   << buildCategoryBlock(ffi_it != agg.end() ? &ffi_it->second :
						       nullptr,
				 ffi_facts)
	   << ",\"boundaries\":" << ffi_facts << "}";

	// last_updated
	ss << ",\"last_updated\":\"" << currentUtcIsoTimestamp() << "\"";
	ss << "}";

	std::string snapshot_json = ss.str();

	// 7. UPSERT into project_state.
	if (!upsertProjectState(store_, project_id, confidence,
				snapshot_json)) {
		return false;
	}

	std::fprintf(stderr,
		     "[model] ProjectState: project_id=%llu "
		     "confidence=%.4f snapshot_bytes=%zu\n",
		     static_cast<unsigned long long>(project_id), confidence,
		     snapshot_json.size());
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
