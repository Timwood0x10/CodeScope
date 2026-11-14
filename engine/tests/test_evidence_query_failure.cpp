// test_evidence_query_failure.cpp — "zero matches" must not be confused with
// "the query failed" (development_plan §17.5).
//
// Before this test the builder returned an empty vector for both cases, so a
// broken schema (or any prepare/step error) silently turned every rule into
// "found nothing" — the exact false confidence P4 forbids.
//
// Covered:
//   1. A healthy database with no matching facts reports ZERO failed queries
//      (that is a legitimate empty result).
//   2. After the facts table disappears, the same call reports FAILED queries,
//      so the caller can refuse to call the project clean.

#include "../src/evidence/evidence_builder.h"
#include "../src/store/store.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/test_evidence_query_failure_%d.db", int(getpid()));
}

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

/// Locate the rule directory from whichever build subdirectory we run in.
static std::string findRulesDir()
{
	static const char *kCandidates[] = {
		"engine/src/rules",
		"../src/rules",
		"../../engine/src/rules",
		"../../../engine/src/rules",
	};
	for (const char *candidate : kCandidates) {
		if (access(candidate, F_OK) == 0)
			return candidate;
	}
	// Deeper build trees (out/build/<config>/...) are common enough that a
	// fixed list would silently stop finding the rules; walk up instead.
	std::string prefix;
	for (int depth = 0; depth < 8; ++depth) {
		std::string candidate = prefix + "engine/src/rules";
		if (access(candidate.c_str(), F_OK) == 0)
			return candidate;
		prefix += "../";
	}
	return {};
}

int main()
{
	initDbPath();
	removeDbFiles();

	store::GraphStore store;
	check(store.open(kDbPath), "open database");
	uint64_t project_id = store.createProject("/test/query-failure",
						   "query-failure");
	check(project_id > 0, "createProject");

	const std::string rules_dir = findRulesDir();
	check(!rules_dir.empty(),
	      "rules directory not found; run from the repository root or a "
	      "build subdirectory");

	evidence::EvidenceBuilder builder(&store);
	builder.loadRules(rules_dir);
	check(!builder.ruleSets().empty(),
	      "no rule sets loaded; the test would not exercise any query");
	check(builder.ruleCount() > 0, "rules must report a non-zero count");

	// Zero loaded rules is the other silent-clean path: nothing ran, so an
	// empty result says nothing about the project. The FFI refuses to answer
	// when ruleCount() is 0, and this is the state it detects.
	evidence::EvidenceBuilder no_rules(&store);
	// Derived from the pid rather than a fixed path: a literal
	// "/nonexistent/..." would make this test pass or fail depending on
	// whether someone happens to create that directory.
	const std::string missing_dir =
		"/tmp/codescope_missing_rules_" + std::to_string(getpid());
	no_rules.loadRules(missing_dir);
	check(no_rules.ruleCount() == 0,
	      "a missing rule directory must load zero rules");

	// ── 1. Healthy database, no matching facts ──────────────────────
	auto empty = builder.buildAll(project_id);
	check(empty.empty(), "no facts inserted, so no evidence is expected");
	check(builder.failedQueries() == 0,
	      "an empty result over a healthy database must NOT count as a "
	      "failure");

	// ── 2. The facts table disappears ───────────────────────────────
	check(store.exec("ALTER TABLE semantic_fact RENAME TO "
			 "semantic_fact_missing"),
	      "rename semantic_fact");
	auto after = builder.buildAll(project_id);
	check(after.empty(), "a failed query cannot produce evidence");
	check(builder.failedQueries() > 0,
	      "a query against a missing table must count as a failure, "
	      "otherwise zero matches and a broken schema are indistinguishable");
	check(!builder.failedRules().empty(),
	      "the names of the failing rules must be reported, not just a "
	      "count");
	check(static_cast<int>(builder.failedRules().size()) ==
		      builder.failedQueries(),
	      "failedQueries() must equal the number of distinct failed rules");

	// The counter must reset per call: restoring the table clears it.
	check(store.exec("ALTER TABLE semantic_fact_missing RENAME TO "
			 "semantic_fact"),
	      "restore semantic_fact");
	auto restored = builder.buildAll(project_id);
	check(restored.empty(), "still no facts after restore");
	check(builder.failedQueries() == 0,
	      "the failure counter must reset on the next build call");
	check(builder.failedRules().empty(),
	      "the failed-rule set must reset on the next build call");

	// ── 3. No store at all ──────────────────────────────────────────
	// A null store cannot run any query, so it must be reported as a failure
	// rather than as "no findings". The sentinel is deliberately outside the
	// rule-name namespace so it cannot collide with a real rule id.
	evidence::EvidenceBuilder broken(nullptr);
	auto none = broken.buildAll(project_id);
	check(none.empty(), "a builder without a store produces no evidence");
	check(broken.failedRules().count("__internal__store_unavailable") == 1,
	      "a missing store must be reported through the failed-rule set");

	store.close();
	removeDbFiles();
	std::printf("\nAll evidence query-failure tests passed.\n");
	return 0;
}
