// engine_evidence_ffi.cpp — Evidence Builder FFI exports (EB4).
//
// Exposes evidence::EvidenceBuilder to the Rust MCP server via a
// single extern "C" entry point:
//
//   char *engine_build_evidence(uint64_t project_id,
//                                const char *category_filter);
//
// The function loads rule files from the directory returned by
// evidence::resolveRulesDir() ($CODESCOPE_RULES_DIR, then the build-
// time default, then the in-tree relative path), runs all rules (or
// one category's rules when `category_filter` is non-empty), and
// returns a JSON array of Evidence objects. The caller MUST release
// the returned pointer via engine_free_string().
//
// Output shape (JSON array of objects):
//   [
//     {
//       "category": "sync",
//       "title": "1 function(s) lock mutex without defer Unlock",
//       "confidence": 1.0,
//       "items": [
//         { "fact_id": 12, "category": "sync", "primitive": "mutex",
//           "kind": "lock", "symbol": "m.Lock",
//           "file": "/src/sync.go", "line": 5,
//           "snippet": "m.Lock (/src/sync.go)" }
//       ]
//     }
//   ]
//
// All errors return a JSON object with an "error" field instead of
// crashing. A null handle (or a store-less instance) returns
//   {"error":"engine not initialized"}.

#include "engine_internal.h"
#include "async_knowledge.h"
#include "constraint/constraint_engine.h"
#include "evidence/evidence_builder.h"
#include "evidence/rule_runner.h"
#include "store/store_identity.h"
#include "platform_win.h"
#include "util/json_writer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ─── Local helpers ──────────────────────────────────────────────

namespace
{

// Serialize one EvidenceItem into the object currently open on `w`.
void writeItem(util::JsonWriter &w, const evidence::EvidenceItem &item)
{
	w.beginObject();
	w.key("fact_id").value(item.fact_id);
	w.key("category").value(item.category);
	w.key("primitive").value(item.primitive);
	w.key("kind").value(item.kind);
	w.key("symbol").value(item.symbol);
	w.key("file").value(item.file);
	w.key("line").value(item.line);
	w.key("snippet").value(item.snippet);
	w.endObject();
}

// Serialize one Evidence as an object; `items` is always emitted as an
// array (empty for the Count combine mode).
void writeEvidence(util::JsonWriter &w, const evidence::Evidence &ev)
{
	w.beginObject();
	w.key("category").value(ev.category);
	w.key("title").value(ev.title);
	w.key("confidence").value(ev.confidence);
	w.key("items").beginArray();
	for (const auto &item : ev.items)
		writeItem(w, item);
	w.endArray();
	w.endObject();
}

} // namespace

// ─── FFI entry point ────────────────────────────────────────────

// Returns JSON array of evidence for a project. Optionally filter by
// category. Caller must free the returned string via
// engine_free_string. Rule files are located by
// evidence::resolveRulesDir() so the result does not depend on the
// process working directory.
char *engine_build_evidence(engine_t handle, uint64_t project_id,
			    const char *category_filter)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!ctx || !ctx->store)
			return dupString(
				"{\"error\":\"engine not initialized\"}");

		// Returning [] here would say "nothing to report" when in fact no rule
		// ever ran, which is the same false assurance as a failed query
		// (development_plan §17.5).
		std::string rules_dir = evidence::resolveRulesDir();
		if (rules_dir.empty())
			return dupString(util::errorEnvelope(
				"ffi", "engine_build_evidence",
				"rule directory not found; cannot report a clean result "
				"without running any rule"));

		evidence::EvidenceBuilder builder(ctx->store.get());
		builder.loadRules(rules_dir);
		if (builder.ruleCount() == 0)
			return dupString(util::errorEnvelope(
				"ffi", "engine_build_evidence",
				"no rule files loaded from '" + rules_dir +
					"'; cannot report a clean result without "
					"running any rule"));
		// A rule rejected for bad metadata is a load failure, not a smaller
		// rule set (development_plan §8.1, §17.5).
		const std::string rejected =
			evidence::rejectedRulesReason(builder.loadReport());
		if (!rejected.empty())
			return dupString(util::errorEnvelope(
				"ffi", "engine_build_evidence", rejected));

		std::vector<evidence::Evidence> evidences;
		if (category_filter && *category_filter) {
			evidences = builder.buildByCategory(project_id,
							    category_filter);
		} else {
			evidences = builder.buildAll(project_id);
		}

		// Zero matches and a failed query both yield an empty list. Returning
		// `[]` would claim "nothing to report" when the facts could not even
		// be read, so report the failure instead of a confident empty answer
		// (development_plan §17.5). The rule names matter: a bare count does
		// not tell the caller whether its rule is among the failures.
		if (builder.failedQueries() > 0) {
			constexpr int kMaxRulesInError = 10;
			std::string names;
			int shown = 0;
			for (const auto &rule : builder.failedRules()) {
				if (shown == kMaxRulesInError) {
					names += ", ...";
					break;
				}
				if (!names.empty())
					names += ", ";
				names += rule;
				++shown;
			}
			return dupString(util::errorEnvelope(
				"ffi", "engine_build_evidence",
				std::to_string(builder.failedQueries()) +
					" rule(s) failed to query facts: " +
					names));
		}

		util::JsonWriter w;
		w.beginArray();
		for (const auto &ev : evidences)
			writeEvidence(w, ev);
		w.endArray();
		return dupString(w.str());
	} catch (const std::exception &e) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_build_evidence", e.what()));
	} catch (...) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_build_evidence", "unknown exception"));
	}
}

// ─── Rule execution (0.3.0 verification model) ──────────────────

namespace
{

/// Analyzer version recorded on every run. Bumped with the engine, so two
/// runs over the same source are distinguishable when the tool changed.
constexpr const char *kAnalyzerVersion = "0.3.0";

/// Run id unique per process and per call.
///
/// Wall-clock alone is not enough: two runs started in the same millisecond
/// would collide on the primary key and the second would be rejected as a
/// duplicate, silently leaving that analysis without a run row.
std::string MakeRunId()
{
	// Millisecond clock plus a process-local counter: two runs in the same
	// millisecond still get different ids, which is what the primary key
	// needs. Cross-process collision is not a concern while a project DB has
	// a single writer (§17.7: runs are produced by the main process).
	static std::atomic<unsigned long long> counter{ 0 };
	const auto since = std::chrono::system_clock::now().time_since_epoch();
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(since)
			.count();
	return "run-" + std::to_string(ms) + "-" + std::to_string(++counter);
}

std::string NowIso()
{
	std::time_t t = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

/// Read a file's content into a string. Returns empty string on failure
/// (a missing .gitignore is a valid "no rules" state, not an error).
static std::string readFileContent(const std::string &path)
{
	std::ifstream f(path);
	if (!f.is_open())
		return {};
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

/// Populate the input digests on a verification run so compare_snapshots
/// can tell whether two runs used the same inputs.
/// - constraint_digest: from the active constraint set; "none-declared"
///   when no constraints are active (non-empty so inputsComparable does
///   not treat two no-constraint runs as "not recorded").
/// - filter_policy_digest: SHA-256 of the effective filtering policy,
///   including .gitignore content, index mode, language filter, and
///   CODESCOPE_EXCLUDE_PATHS. A change to any of these alters which
///   files are indexed, so two runs with different policies must not
///   be reported as "same inputs".
void populateInputDigests(store::VerificationRun &run, store::GraphStore &store,
			  uint64_t project_id)
{
	std::string constraint_digest =
		constraint::activeConstraintDigest(store, project_id);
	run.constraint_digest = constraint_digest.empty() ? "none-declared" :
							    constraint_digest;

	// Read the filter policy digest that was stored at index time.
	// This is the authoritative value — it captures the FilterPolicy
	// that was actually in effect when the files table was populated,
	// including the language filter, index mode, .gitignore rules, and
	// CODESCOPE_EXCLUDE_PATHS patterns loaded into the FilterPolicy
	// object. Re-hashing .gitignore at run time (the old approach) was
	// wrong in two ways: (1) editing .gitignore without re-indexing
	// changed the digest even though the indexed files were identical,
	// and (2) two indexes with different language filters but the same
	// .gitignore produced the same digest because the language filter
	// was not captured.
	std::string stored_fpd = store.getFilterPolicyDigest(project_id);
	if (!stored_fpd.empty()) {
		run.filter_policy_digest = stored_fpd;
		return;
	}

	// Fallback: no stored digest (pre-migration DB or index did not
	// reach postParsePhase). Re-hash from the environment + .gitignore
	// so compare_snapshots still has a value to compare.
	std::ostringstream policy;

	const char *exclude_env = std::getenv("CODESCOPE_EXCLUDE_PATHS");
	policy << "EXCLUDE="
	       << (exclude_env && *exclude_env ? exclude_env : "no-exclusions")
	       << "\n";

	const char *index_mode = std::getenv("CODESCOPE_INDEX_MODE");
	policy << "MODE=" << (index_mode && *index_mode ? index_mode : "normal")
	       << "\n";

	std::string root = store.getProjectRootPath(project_id);
	if (!root.empty()) {
		std::string gitignore_path = root + "/.gitignore";
		std::string gitignore_content = readFileContent(gitignore_path);
		policy << "GITIGNORE=" << store::Sha256Hex(gitignore_content)
		       << "\n";

		std::string csignore_path = root + "/.codescopeignore";
		std::string csignore_content = readFileContent(csignore_path);
		policy << "CSIGNORE=" << store::Sha256Hex(csignore_content)
		       << "\n";
	}

	run.filter_policy_digest = store::Sha256Hex(policy.str());
}

} // namespace

/// Aggregate of one batch of rule executions.
struct RuleBatchStats {
	int executed = 0;
	int failed = 0;
	/// Rules that could not produce evidence at all (e.g. Count combine).
	/// Separate from `failed`: nothing went wrong, the rule just has no
	/// location to report, and it must not be counted as a clean execution.
	int skipped = 0;
	int evidence_rows = 0;
	int findings = 0;
	std::string first_error;
};

/// Run every rule in `sets` against the stored facts.
///
/// A failing rule is counted and skipped, not propagated: one broken rule must
/// not hide the results of the others (development_plan §17.8 acceptance 6).
/// Split out of engine_run_rules() to keep that function inside its size limit.
RuleBatchStats runAllRules(store::GraphStore *store,
			   const std::string &rules_dir, uint64_t project_id,
			   const std::string &run_id,
			   const std::vector<evidence::RuleSet> &sets)
{
	RuleBatchStats stats;
	for (const auto &set : sets) {
		for (const auto &rule : set.rules) {
			evidence::RuleRunOutcome out = evidence::runRuleToStore(
				store, rules_dir, project_id, run_id,
				rule.name);
			if (out.skipped) {
				++stats.skipped;
				continue;
			}
			if (!out.executed) {
				++stats.failed;
				if (stats.first_error.empty())
					stats.first_error = out.error;
				continue;
			}
			++stats.executed;
			stats.evidence_rows +=
				static_cast<int>(out.evidence_rows);
			stats.findings += static_cast<int>(out.findings);
		}
	}
	return stats;
}

/// Digest of the rule set that actually ran.
///
/// It covers what a rule DOES, not just its name: editing needs, level or
/// severity changes the input, so two runs cannot claim the same rule set on the
/// strength of identical rule names alone.
std::string RuleSetDigest(const std::vector<evidence::RuleSet> &sets)
{
	std::string text;
	for (const auto &set : sets)
		for (const auto &rule : set.rules) {
			text += rule.name + "@" + rule.version + " " +
				evidence::evidenceLevelToString(
					rule.evidence_level) +
				" sev=" + std::to_string(rule.output.severity) +
				" combine=" +
				evidence::combineModeToString(rule.combine) +
				"\n";
			// `optional` decides whether a need suppresses or selects a
			// finding, so it is part of what the rule does: without it, three
			// genuinely different rules share one digest.
			for (const auto &need : rule.needs)
				text += need.category + "/" + need.primitive +
					"/" + need.kind +
					(need.optional ? "?" : "") + ";";
			// A metric rule has no needs; its strength lives in the
			// threshold, which must change the digest too (§13.2:
			// changing a threshold must change the evidence version).
			if (rule.type == evidence::RuleType::Metric)
				text += std::string("metric=") +
					evidence::metricToString(
						rule.metric.metric) +
					evidence::metricOpToString(
						rule.metric.op) +
					":" +
					std::to_string(rule.metric.threshold);
			text += "\n";
		}
	return store::Sha256Hex(text);
}

/// Distinct findings the run produced, falling back to the accumulated count
/// when the query fails (the envelope must still answer something true-ish).
int64_t countRunFindings(store::GraphStore &store, const std::string &run_id,
			 int fallback)
{
	const char *sql = "SELECT COUNT(DISTINCT finding_key) FROM evidence "
			  "WHERE run_id = ? AND finding_key <> ''";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return fallback;
	sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
	int64_t count = fallback;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

// Execute every loaded rule once, persisting the run, its evidence and its
// findings. This is the production entry point the unified model was missing:
// until now runRuleToStore() was reachable from tests only.
//
// A rule that fails does not stop the batch — it is recorded as failed and the
// run is marked partial, so one broken rule cannot hide the other results
// (development_plan §17.8 acceptance 6).
char *engine_run_rules(engine_t handle, uint64_t project_id,
		       const char *revision)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!ctx || !ctx->store)
			return dupString(
				util::errorEnvelope("ffi", "engine_run_rules",
						    "engine not initialized"));

		const std::string rules_dir = evidence::resolveRulesDir();
		if (rules_dir.empty())
			return dupString(util::errorEnvelope(
				"ffi", "engine_run_rules",
				"rule directory not found; a run without rules "
				"would look like a clean project"));

		evidence::EvidenceBuilder builder(ctx->store.get());
		builder.loadRules(rules_dir);
		if (builder.ruleCount() == 0)
			return dupString(util::errorEnvelope(
				"ffi", "engine_run_rules",
				"no rule files loaded from '" + rules_dir +
					"'"));
		// A rejected rule means the rule set is not the one the project
		// declared. Running the rest would report a cleaner project than was
		// actually checked (development_plan §8.1, §17.5).
		const std::string rejected =
			evidence::rejectedRulesReason(builder.loadReport());
		if (!rejected.empty())
			return dupString(util::errorEnvelope(
				"ffi", "engine_run_rules", rejected));

		// The run pins the source state. A caller without a VCS passes
		// nothing and gets the explicit WORKTREE sentinel: an empty revision
		// satisfies NOT NULL but makes two different trees indistinguishable.
		store::VerificationRun run;
		run.run_id = MakeRunId();
		run.project_id = project_id;
		run.source.revision = (revision && *revision) ? revision :
								"WORKTREE";
		run.source.worktree_digest =
			ctx->store->worktreeDigest(project_id);
		if (run.source.worktree_digest.empty())
			return dupString(util::errorEnvelope(
				"ffi", "engine_run_rules",
				"worktree digest failed: " +
					ctx->store->error()));
		run.source.analyzer_version = kAnalyzerVersion;

		run.rule_set_digest = RuleSetDigest(builder.ruleSets());

		// Populate constraint_digest and filter_policy_digest so
		// compare_snapshots can tell whether two runs used the same
		// inputs. See populateInputDigests for the sentinel logic.
		populateInputDigests(run, *ctx->store, project_id);

		run.profile = "default";
		run.started_at = NowIso();
		if (!ctx->store->insertVerificationRun(run))
			return dupString(util::errorEnvelope(
				"ffi", "engine_run_rules",
				"insert run failed: " + ctx->store->error()));

		const RuleBatchStats stats =
			runAllRules(ctx->store.get(), rules_dir, project_id,
				    run.run_id, builder.ruleSets());
		const int executed = stats.executed;
		const int failed = stats.failed;
		const bool complete = failed == 0;
		const store::RunStatus status =
			complete ? store::RunStatus::Completed :
				   store::RunStatus::Partial;
		const std::string run_error = complete ? std::string() :
							 stats.first_error;
		// A run left at "running" is worse than a failed close: every later
		// reader would treat it as still in progress. Report it.
		if (!ctx->store->finishVerificationRun(run.run_id, status,
						       run_error))
			return dupString(util::errorEnvelope(
				"ffi", "engine_run_rules",
				"could not close the run: " +
					ctx->store->error()));

		util::JsonWriter w;
		w.beginObject();
		w.key("ok").value(true);
		w.key("run_id").value(run.run_id);
		w.key("rules").value(executed + failed + stats.skipped);
		w.key("executed").value(executed);
		w.key("skipped").value(stats.skipped);
		w.key("failed").value(failed);
		w.key("evidence").value(stats.evidence_rows);
		// Distinct findings, not evidence rows: one finding backed by two
		// locations (a declaration and a definition) added 2 here while
		// `finding` grew by 1, so the envelope disagreed with the briefing.
		w.key("findings")
			.value(countRunFindings(*ctx->store, run.run_id,
						stats.findings));
		w.key("status").value(complete ? "completed" : "partial");
		w.endObject();
		return dupString(w.str());
	} catch (const std::exception &e) {
		return dupString(util::errorEnvelope("ffi", "engine_run_rules",
						     e.what()));
	} catch (...) {
		return dupString(util::errorEnvelope("ffi", "engine_run_rules",
						     "unknown exception"));
	}
}
