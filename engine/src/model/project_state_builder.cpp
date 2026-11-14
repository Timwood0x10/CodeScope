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
// SQL query helpers (countRows, detectFrameworks, etc.) live in
// project_state_queries.cpp; this file keeps scoring and JSON
// serialisation so the "do not copy old scoring logic" directive
// (development_plan §12.2) is respected.
//
// All sqlite3_stmt handles are managed by an RAII guard so they are
// finalized on every exit path. The store pointer is borrowed; null
// is checked at every public entry point and returns false / empty
// (no crash). Empty state tables are not errors — the snapshot simply
// omits the corresponding key (or sets its score to a default).

#include "project_state_builder.h"
#include "project_state_internal.h"
#include "project_state_queries.h"
#include "util/json_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// ─── Local helpers (scoring + JSON serialisation) ──────────────

namespace
{

using detail::colText;
using detail::StmtGuard;

std::string escapeJson(const std::string &s)
{
	return util::jsonEscapeString(s);
}

std::string fmtDouble(double v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.4f", v);
	std::string s(buf);
	while (!s.empty() && s.back() == '0')
		s.pop_back();
	if (!s.empty() && s.back() == '.')
		s.pop_back();
	if (s.empty() || s == "-0")
		s = "0";
	return s;
}

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

struct CategoryAgg {
	int issue_count = 0;
	std::vector<std::string> details;
};

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

int countInspectorsRan(const std::unordered_map<std::string, CategoryAgg> &agg)
{
	return static_cast<int>(agg.size());
}

} // namespace

// ─── ProjectStateBuilder public API ──────────────────────────────

std::string
categoryBlockFor(const std::unordered_map<std::string, CategoryAgg> &agg,
		 const char *key, int64_t fact_count)
{
	auto it = agg.find(key);
	return buildCategoryBlock(it != agg.end() ? &it->second : nullptr,
				  fact_count);
}

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

struct SnapshotScale {
	int64_t workflow_total = 0;
	int64_t steps_done = 0;
	int64_t steps_total = 0;
	int64_t dead_entities = 0;
	int64_t total_entities = 0;
};

struct CategoryTotals {
	int64_t sync = 0;
	int64_t memory = 0;
	int64_t error = 0;
	int64_t pattern = 0;
	int64_t ffi = 0;
	int64_t framework = 0;
};

struct SeverityCounts {
	int info = 0;
	int warning = 0;
	int error = 0;
};

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
			out.info += kv.second;
			break;
		}
	}
	return out;
}

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
