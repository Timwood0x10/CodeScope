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

} // namespace

/// Aggregate of one batch of rule executions.
struct RuleBatchStats {
	int executed = 0;
	int failed = 0;
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

		// Digest of the rule set actually executed, so two runs can be told
		// apart even when the source did not change.
		// The digest covers what a rule DOES, not just its name: renaming
		// nothing but editing a rule's fact needs would otherwise leave two
		// different rule sets with the same digest.
		std::string rule_names;
		for (const auto &set : builder.ruleSets())
			for (const auto &rule : set.rules) {
				rule_names += rule.name + "\n";
				for (const auto &need : rule.needs)
					rule_names += need.category + "/" +
						      need.primitive + "/" +
						      need.kind + ";";
				rule_names += "\n";
			}
		run.rule_set_digest = store::Sha256Hex(rule_names);

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
		w.key("rules").value(executed + failed);
		w.key("executed").value(executed);
		w.key("failed").value(failed);
		w.key("evidence").value(stats.evidence_rows);
		w.key("findings").value(stats.findings);
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
