// test_evidence_ffi_rules.cpp — "no rule ran" must not look like a clean
// project (development_plan §17.8 acceptance 5).
//
// engine_build_evidence used to answer `[]` whenever it ended up with zero
// rules: an empty array reads as "nothing to report" while in fact no rule
// executed. The FFI now refuses that answer, and this test is the only one
// that exercises the branch — the builder-level tests cannot see it, because
// the decision lives in engine_build_evidence.
//
// Why the case is "unparsable rule file" rather than "missing directory":
// resolveRulesDir() skips any candidate without a *.json file and falls back
// to the build-time default. A missing or empty directory therefore never
// reaches the FFI as an empty path, so the only reachable zero-rule state is a
// directory whose files all fail to load.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

static char kDbPath[160];
static char kBrokenRulesDir[160];

static void initPaths()
{
	std::snprintf(kDbPath, sizeof(kDbPath), "/tmp/codescope_rules_ffi_%d.db",
		      int(getpid()));
	std::snprintf(kBrokenRulesDir, sizeof(kBrokenRulesDir),
		      "/tmp/codescope_broken_rules_%d", int(getpid()));
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

/// The answer must be an error envelope, not an empty array.
static void expectRuleError(const char *label, char *result)
{
	if (result == nullptr) {
		std::fprintf(stderr, "FAIL %s: null pointer\n", label);
		std::exit(1);
	}
	const std::string json(result);
	engine_free_string(result);

	if (json == "[]") {
		std::fprintf(stderr,
			     "FAIL %s: returned [] — no rule ran, but the answer "
			     "claims the project is clean\n",
			     label);
		std::exit(1);
	}
	if (json.find("\"error\"") == std::string::npos) {
		std::fprintf(stderr, "FAIL %s: no error field in %.120s\n", label,
			     json.c_str());
		std::exit(1);
	}
	std::printf("  [PASS] %s\n", label);
}

int main()
{
	initPaths();
	removeDbFiles();
	std::filesystem::remove_all(kBrokenRulesDir);

	engine_t engine = engine_create(kDbPath);
	if (!engine) {
		std::fprintf(stderr, "FAIL: engine_create(%s)\n", kDbPath);
		return 1;
	}

	// A directory that yields zero loaded rules: the resolver is satisfied
	// (the .json exists) but every file fails to parse.
	std::filesystem::create_directories(kBrokenRulesDir);
	{
		std::ofstream broken(std::string(kBrokenRulesDir) + "/broken.json");
		broken << "{ this is not valid json ";
	}
	setenv("CODESCOPE_RULES_DIR", kBrokenRulesDir, 1);
	expectRuleError("rule directory with no loadable rules",
			engine_build_evidence(engine, 1, ""));

	unsetenv("CODESCOPE_RULES_DIR");
	std::filesystem::remove_all(kBrokenRulesDir);

	engine_destroy(engine);
	removeDbFiles();
	std::printf("\nAll evidence FFI rule-set tests passed.\n");
	return 0;
}
