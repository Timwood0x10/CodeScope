// test_rule_metadata.cpp — rule metadata is a gate, not a comment.
//
// development_plan §8.1 requires every rule to declare its evidence level and
// says a rule without one must be refused; §16.11 requires id/version/
// category/severity/enabled; §10.2 fixes severity to the persisted 0..4 scale.
// The loader used to accept all of this silently: a rule without a level
// became L1 by default, which is exactly how an L2 heuristic ends up quoted as
// an L1 fact, and an unknown combine spelling quietly became "collect".
//
// Covered:
//   1. a valid L1 (collect) and L2 (absence-based) rule load with their
//      declared level and version intact;
//   2. a missing or unknown evidence_level is rejected;
//   3. an L1 rule built on an absence-based combine is rejected;
//   4. a severity outside 0..4 is rejected;
//   5. a missing version and an unknown combine are rejected;
//   6. enabled:false is reported as disabled, not loaded as a smaller set;
//   7. the shipped rule directory loads with zero rejections and keeps the
//      L1 ⇒ collect invariant;
//   8. engine_run_rules writes the declared level into rule_exec and records a
//      Count rule as skipped_not_applicable instead of "executed, no findings".

#include "../include/engine.h"
#include "../src/evidence/rule.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static char kDbPath[160];
static char kRulesDir[160];

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void initPaths()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_rule_meta_%d.db", int(getpid()));
	std::snprintf(kRulesDir, sizeof(kRulesDir),
		      "/tmp/codescope_rule_meta_%d", int(getpid()));
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

static void writeFile(const std::string &name, const char *body)
{
	std::ofstream out(std::string(kRulesDir) + "/" + name);
	out << body;
}

/// One rule object; `extra` carries the metadata under test.
static std::string ruleBody(const char *name, const char *extra,
			    const char *combine = "collect")
{
	return std::string("    {\n        \"name\": \"") + name + "\",\n" +
	       extra +
	       "        \"need\": [\n"
	       "            { \"category\": \"pattern\", "
	       "\"primitive\": \"todo\", \"kind\": \"marker\" }\n"
	       "        ],\n        \"combine\": \"" +
	       combine +
	       "\",\n        \"output\": { \"severity\": 2, "
	       "\"title\": \"t\" }\n    }\n";
}

static void expectRejected(const evidence::RuleLoadReport &report,
			   std::size_t expected, const char *needle)
{
	check(report.rejected.size() == expected,
	      "unexpected number of rejected rules");
	bool found = false;
	for (const auto &err : report.rejected)
		if (err.reason.find(needle) != std::string::npos)
			found = true;
	std::string msg = std::string("no rejection mentions '") + needle + "'";
	check(found, msg.c_str());
}

/// Cases 1-6: the loader refuses bad metadata and keeps good rules.
static void checkLoaderGates()
{
	std::filesystem::remove_all(kRulesDir);
	std::filesystem::create_directories(kRulesDir);

	writeFile("ok.json",
		  ("{\n  \"category\": \"pattern\",\n  \"rules\": [\n" +
		   ruleBody("ok_l1", "        \"version\": \"1\",\n"
				     "        \"evidence_level\": \"L1\",\n") +
		   ",\n" +
		   ruleBody("ok_l2",
			    "        \"version\": \"2\",\n"
			    "        \"evidence_level\": \"L2\",\n",
			    "missing_match") +
		   "  ]\n}\n")
			  .c_str());
	writeFile("no_level.json",
		  ("{\n  \"rules\": [\n" +
		   ruleBody("no_level", "        \"version\": \"1\",\n") +
		   "  ]\n}\n")
			  .c_str());
	writeFile("bad_level.json",
		  ("{\n  \"rules\": [\n" +
		   ruleBody("bad_level",
			    "        \"version\": \"1\",\n"
			    "        \"evidence_level\": \"L4\",\n") +
		   "  ]\n}\n")
			  .c_str());
	writeFile("l1_absence.json",
		  ("{\n  \"rules\": [\n" +
		   ruleBody("l1_absence",
			    "        \"version\": \"1\",\n"
			    "        \"evidence_level\": \"L1\",\n",
			    "missing_match") +
		   "  ]\n}\n")
			  .c_str());
	writeFile("bad_severity.json",
		  "{\n  \"rules\": [\n"
		  "    {\n        \"name\": \"bad_severity\",\n"
		  "        \"version\": \"1\",\n"
		  "        \"evidence_level\": \"L1\",\n"
		  "        \"need\": [ { \"category\": \"pattern\", "
		  "\"primitive\": \"todo\", \"kind\": \"marker\" } ],\n"
		  "        \"combine\": \"collect\",\n"
		  "        \"output\": { \"severity\": 9, \"title\": \"t\" }\n"
		  "    }\n  ]\n}\n");
	writeFile("no_version.json",
		  ("{\n  \"rules\": [\n" +
		   ruleBody("no_version",
			    "        \"evidence_level\": \"L1\",\n") +
		   "  ]\n}\n")
			  .c_str());
	writeFile("bad_combine.json",
		  ("{\n  \"rules\": [\n" +
		   ruleBody("bad_combine",
			    "        \"version\": \"1\",\n"
			    "        \"evidence_level\": \"L2\",\n",
			    "collectt") +
		   "  ]\n}\n")
			  .c_str());
	writeFile(
		"frac_severity.json",
		"{\n  \"rules\": [\n"
		"    {\n        \"name\": \"frac_severity\",\n"
		"        \"version\": \"1\",\n"
		"        \"evidence_level\": \"L1\",\n"
		"        \"need\": [ { \"category\": \"pattern\", "
		"\"primitive\": \"todo\", \"kind\": \"marker\" } ],\n"
		"        \"combine\": \"collect\",\n"
		"        \"output\": { \"severity\": 4.9, \"title\": \"t\" }\n"
		"    }\n  ]\n}\n");
	writeFile("frac_threshold.json",
		  "{\n  \"rules\": [\n"
		  "    {\n        \"name\": \"frac_threshold\",\n"
		  "        \"version\": \"1\",\n"
		  "        \"evidence_level\": \"L1\",\n"
		  "        \"type\": \"metric\",\n"
		  "        \"metric\": { \"name\": \"function_lines\", "
		  "\"op\": \"gt\", \"threshold\": 120.5 },\n"
		  "        \"output\": { \"severity\": 2, \"title\": \"t\" }\n"
		  "    }\n  ]\n}\n");
	writeFile("disabled.json",
		  (std::string("{\n  \"rules\": [\n") +
		   "    {\n        \"name\": \"off\",\n"
		   "        \"version\": \"1\",\n"
		   "        \"evidence_level\": \"L1\",\n"
		   "        \"enabled\": false,\n"
		   "        \"need\": [ { \"category\": \"pattern\", "
		   "\"primitive\": \"todo\", \"kind\": \"marker\" } ],\n"
		   "        \"combine\": \"collect\",\n"
		   "        \"output\": { \"severity\": 2, \"title\": \"t\" }\n"
		   "    }\n  ]\n}\n")
			  .c_str());

	evidence::RuleLoader loader;
	evidence::RuleLoadReport report;
	const auto sets = loader.loadFromDirectory(kRulesDir, &report);

	std::size_t loaded = 0;
	for (const auto &set : sets)
		loaded += set.rules.size();
	check(loaded == 2, "the two valid rules must load");
	check(report.rejected.size() == 8,
	      "eight rules must be rejected, not silently dropped");
	check(report.disabled.size() == 1, "enabled:false must be reported");

	expectRejected(report, 8, "missing 'evidence_level'");
	expectRejected(report, 8, "unknown evidence_level");
	expectRejected(report, 8, "cannot back L1");
	expectRejected(report, 8, "severity must be an integer in 0..4");
	expectRejected(report, 8, "missing 'version'");
	expectRejected(report, 8, "must be one of collect");
	// 4.9 used to truncate to 4 and pass the range check.
	expectRejected(report, 8, "threshold' in 0..1e9");

	const evidence::Rule *l1 = nullptr;
	const evidence::Rule *l2 = nullptr;
	for (const auto &set : sets)
		for (const auto &rule : set.rules) {
			if (rule.name == "ok_l1")
				l1 = &rule;
			if (rule.name == "ok_l2")
				l2 = &rule;
		}
	check(l1 && l2, "both valid rules must be addressable");
	check(l1->evidence_level == store::EvidenceLevel::L1,
	      "L1 must survive the loader");
	check(l2->evidence_level == store::EvidenceLevel::L2,
	      "L2 must survive the loader");
	check(l1->version == "1" && l2->version == "2",
	      "the declared version must survive the loader");
	std::printf("  [PASS] loader gates\n");
}

/// Case 7: the shipped rules keep the same invariants.
static void checkShippedRules()
{
	unsetenv("CODESCOPE_RULES_DIR");
	const std::string dir = evidence::resolveRulesDir();
	check(!dir.empty(), "shipped rule directory must resolve");

	evidence::RuleLoader loader;
	evidence::RuleLoadReport report;
	const auto sets = loader.loadFromDirectory(dir, &report);
	check(report.rejected.empty(),
	      "shipped rules must all declare valid metadata");

	std::size_t count = 0;
	for (const auto &set : sets)
		for (const auto &rule : set.rules) {
			++count;
			check(!rule.version.empty(),
			      "shipped rule has no version");
			check(rule.output.severity >= 0 &&
				      rule.output.severity <= 4,
			      "shipped rule severity outside 0..4");
			if (rule.evidence_level == store::EvidenceLevel::L1)
				check(rule.combine ==
					      evidence::CombineMode::Collect,
				      "an L1 shipped rule uses an "
				      "absence-based combine");
		}
	check(count > 0, "shipped rules must load");
	std::printf("  [PASS] shipped rules (%zu) keep L1 => collect\n", count);
}

static std::string scalarText(const char *sql)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(kDbPath, &db) != SQLITE_OK)
		return {};
	sqlite3_stmt *st = nullptr;
	std::string out;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
	    sqlite3_step(st) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (p)
			out = p;
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	return out;
}

static long scalar(const char *sql)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open(kDbPath, &db) != SQLITE_OK)
		return -1;
	sqlite3_stmt *st = nullptr;
	long value = -1;
	if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
	    sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	sqlite3_close(db);
	return value;
}

/// Case 8: the declared level reaches rule_exec, and a Count rule is skipped.
static void checkRunRecordsLevel()
{
	std::filesystem::remove_all(kRulesDir);
	std::filesystem::create_directories(kRulesDir);
	writeFile("run.json",
		  ("{\n  \"rules\": [\n" +
		   ruleBody("run_l1", "        \"version\": \"1\",\n"
				      "        \"evidence_level\": \"L1\",\n") +
		   ",\n" +
		   ruleBody("run_l2",
			    "        \"version\": \"1\",\n"
			    "        \"evidence_level\": \"L2\",\n",
			    "missing_match") +
		   ",\n" +
		   ruleBody("run_count",
			    "        \"version\": \"1\",\n"
			    "        \"evidence_level\": \"L2\",\n",
			    "count") +
		   "  ]\n}\n")
			  .c_str());
	setenv("CODESCOPE_RULES_DIR", kRulesDir, 1);

	removeDbFiles();
	engine_t engine = engine_create(kDbPath);
	check(engine != nullptr, "engine_create");

	char *raw = engine_run_rules(engine, 1, nullptr);
	check(raw != nullptr, "engine_run_rules must not return null");
	const std::string json(raw);
	engine_free_string(raw);
	check(json.find("\"ok\":true") != std::string::npos,
	      "run must report ok");
	check(json.find("\"executed\":2") != std::string::npos,
	      "two rules execute; the Count rule does not");
	check(json.find("\"skipped\":1") != std::string::npos,
	      "the Count rule must be reported as skipped");
	check(json.find("\"failed\":0") != std::string::npos,
	      "skipping is not failing");

	check(scalar("SELECT COUNT(*) FROM rule_exec "
		     "WHERE evidence_level = 'L1';") == 1,
	      "the L1 rule must record its declared level");
	check(scalar("SELECT COUNT(*) FROM rule_exec "
		     "WHERE evidence_level = 'L2';") == 2,
	      "the L2 rules must record their declared level");
	check(scalar("SELECT COUNT(*) FROM rule_exec WHERE status = "
		     "'skipped_not_applicable';") == 1,
	      "the Count rule must be recorded as skipped_not_applicable");
	check(scalar("SELECT COUNT(*) FROM rule_exec WHERE "
		     "rule_id = 'run_count' AND skipped_reason = '';") == 0,
	      "a skipped rule must carry its reason");

	engine_destroy(engine);
	unsetenv("CODESCOPE_RULES_DIR");
	std::printf("  [PASS] rule_exec records level, Count is skipped\n");
}

/// Case 9: the rule-set digest covers what a rule does, not only its name.
///
/// `combine` and `need.optional` decide whether a rule emits a finding, no
/// finding or a skip, so three such rules had one digest and rule changes could
/// not be told apart.
static void checkDigestTracksBehaviour()
{
	std::filesystem::remove_all(kRulesDir);
	std::filesystem::create_directories(kRulesDir);
	setenv("CODESCOPE_RULES_DIR", kRulesDir, 1);

	auto writeVariant = [](const char *need, const char *combine) {
		const std::string json =
			std::string("{\n  \"rules\": [\n") +
			"    {\n        \"name\": \"v\",\n"
			"        \"version\": \"1\",\n"
			"        \"evidence_level\": \"L2\",\n"
			"        \"need\": [ " +
			need + " ],\n        \"combine\": \"" + combine +
			"\",\n        \"output\": { \"severity\": 2, "
			"\"title\": \"t\" }\n"
			"    }\n  ]\n}\n";
		writeFile("var.json", json.c_str());
	};

	auto digestOf = [](engine_t engine) {
		char *raw = engine_run_rules(engine, 1, nullptr);
		check(raw != nullptr, "engine_run_rules must not return null");
		const std::string json(raw);
		engine_free_string(raw);
		const std::string marker = "\"run_id\":\"";
		const std::size_t at = json.find(marker);
		check(at != std::string::npos, "the run must report its id");
		const std::size_t start = at + marker.size();
		const std::string run_id =
			json.substr(start, json.find('"', start) - start);
		return scalarText(
			("SELECT rule_set_digest FROM verification_run "
			 "WHERE run_id='" +
			 run_id + "'")
				.c_str());
	};

	removeDbFiles();
	engine_t engine = engine_create(kDbPath);
	check(engine != nullptr, "engine_create");

	const char *plain = "{\"category\":\"pattern\",\"primitive\":"
			    "\"todo\",\"kind\":\"marker\"}";
	const char *exclusion = "{\"category\":\"pattern\",\"primitive\":"
				"\"todo\",\"kind\":\"marker\","
				"\"optional\":true}";

	writeVariant(plain, "collect");
	const std::string selected = digestOf(engine);
	writeVariant(exclusion, "collect");
	const std::string excluded = digestOf(engine);
	writeVariant(plain, "missing_match");
	const std::string absence = digestOf(engine);

	check(!selected.empty(), "digest must be readable");
	check(selected != excluded, "need.optional must change the digest");
	check(excluded != absence, "combine must change the digest");

	engine_destroy(engine);
	unsetenv("CODESCOPE_RULES_DIR");
	std::printf("  [PASS] rule_set_digest tracks behaviour\n");
}

int main()
{
	initPaths();
	checkLoaderGates();
	checkShippedRules();
	checkRunRecordsLevel();
	checkDigestTracksBehaviour();

	std::filesystem::remove_all(kRulesDir);
	removeDbFiles();
	std::printf("\nAll rule metadata tests passed.\n");
	return 0;
}
