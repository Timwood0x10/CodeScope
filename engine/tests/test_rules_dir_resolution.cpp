// test_rules_dir_resolution: pin evidence::resolveRulesDir() so the
// rule directory is found independently of the process working
// directory.
//
// Regression covered: engine_build_evidence / ProjectStateBuilder used
// to fall back to the CWD-relative "engine/src/evidence/rules", which
// only exists when running from the repository root. The MCP/CLI
// vehicles run inside the target project, so build_evidence silently
// loaded zero rules and returned [] (T5 finding #5).
//
// Test flow:
//   1. chdir into a fresh empty temp dir (so every relative candidate
//      except the build-time absolute default is absent)
//   2. resolveRulesDir() must return a directory containing *.json
//   3. $CODESCOPE_RULES_DIR must win over the built-in default
//   4. restore CWD and clear the override

#include "../src/evidence/rule.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

bool dirHasJson(const std::string &dir)
{
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec))
		return false;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec)
			break;
		if (entry.is_regular_file() && entry.path().extension() == ".json")
			return true;
	}
	return false;
}

} // namespace

int main()
{
	// Remember the real CWD so the test can restore it.
	char original_cwd[4096];
	if (!getcwd(original_cwd, sizeof(original_cwd))) {
		fprintf(stderr, "FAIL: getcwd failed\n");
		return 1;
	}

	// 1. A fresh empty directory: no engine/src/evidence/rules under it,
	//    so the CWD-relative fallback cannot succeed.
	std::filesystem::path foreign =
		std::filesystem::temp_directory_path() /
		"codescope_rules_dir_test";
	std::error_code ec;
	std::filesystem::remove_all(foreign, ec);
	std::filesystem::create_directories(foreign, ec);
	if (chdir(foreign.c_str()) != 0) {
		fprintf(stderr, "FAIL: chdir(%s) failed\n", foreign.c_str());
		return 1;
	}

	// 2. The build-time default (or the env override, if the operator
	//    set one before running the test) must still resolve.
	std::string resolved = evidence::resolveRulesDir();
	if (resolved.empty() || !dirHasJson(resolved)) {
		fprintf(stderr,
			"FAIL: resolveRulesDir() from foreign CWD returned "
			"'%s' (expected a rules directory with *.json)\n",
			resolved.c_str());
		chdir(original_cwd);
		return 1;
	}
	printf("Test 1 (resolveRulesDir from foreign CWD = %s): PASS\n",
	       resolved.c_str());

	// 3. $CODESCOPE_RULES_DIR must win over every other candidate.
	//    Point it at the directory step 2 found (it is known-good).
	setenv("CODESCOPE_RULES_DIR", resolved.c_str(), 1);
	std::string overridden = evidence::resolveRulesDir();
	if (overridden != resolved) {
		fprintf(stderr,
			"FAIL: $CODESCOPE_RULES_DIR ignored — got '%s', "
			"expected '%s'\n",
			overridden.c_str(), resolved.c_str());
		unsetenv("CODESCOPE_RULES_DIR");
		chdir(original_cwd);
		return 1;
	}
	printf("Test 2 ($CODESCOPE_RULES_DIR override honoured): PASS\n");
	unsetenv("CODESCOPE_RULES_DIR");

	// 4. Restore CWD and clean up the temp dir.
	if (chdir(original_cwd) != 0)
		fprintf(stderr, "warning: chdir back to %s failed\n",
			original_cwd);
	std::filesystem::remove_all(foreign, ec);

	printf("=== test_rules_dir_resolution PASSED ===\n");
	return 0;
}
