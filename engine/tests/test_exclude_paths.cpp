// test_exclude_paths: verify the CODESCOPE_EXCLUDE_PATHS env var and
// FilterPolicy::loadExcludeEnv().
//
// Tests the env-var-driven exclude path mechanism that allows users to
// trim non-core directories at index time without modifying source or
// .gitignore. Patterns are comma-separated globs matched against the
// project-relative path via globMatch().
//
// Covers six scenarios:
//   1. Basic exclusion: "test/*,docs/*" skips test/foo.cpp, allows src/main.cpp.
//   2. Whitespace trimming: " test/* , docs/* " is equivalent to the trimmed form.
//   3. Unset env var: no user exclusions (only built-in defaults apply).
//   4. Directory skipping: "vendor/*" skips the vendor/ directory entry.
//   5. Non-default path: a custom dir not in the built-in skip list is excluded
//      only when the env var is set — isolates the env-var effect from defaults.
//   6. Escaped comma: "a\,b/**" is ONE pattern (a directory named "a,b"), while
//      an unescaped comma still separates patterns (D2-7).
#include "../src/filter_policy.h"

#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <string>

/// Helper: set env var, create policy, load env, return the policy.
/// Each test gets a fresh FilterPolicy because loadExcludeEnv APPENDS to
/// exclude_patterns_ (it does not clear existing entries).
static FilterPolicy makePolicyWithEnv(const char *env_value)
{
	if (env_value)
		setenv("CODESCOPE_EXCLUDE_PATHS", env_value, 1);
	else
		unsetenv("CODESCOPE_EXCLUDE_PATHS");
	FilterPolicy fp;
	fp.loadExcludeEnv();
	return fp;
}

int main()
{
	// ── 1. Basic exclusion ────────────────────────────────────────
	//
	// "test/*,docs/*" must skip test/foo.cpp (matches "test/*") and
	// allow src/main.cpp (matches neither pattern).
	//
	// Note: "test" is also in the built-in skip_dirs, so test/foo.cpp
	// is skipped regardless. The src/main.cpp case is the true negative
	// test for the env var.
	{
		FilterPolicy fp = makePolicyWithEnv("test/*,docs/*");
		CHECK(fp.shouldSkipEntry("test/foo.cpp", false));
		CHECK(!fp.shouldSkipEntry("src/main.cpp", false));
		printf("  [PASS] basic: test/* skipped, src/main.cpp allowed\n");
	}

	// ── 2. Whitespace trimming ────────────────────────────────────
	//
	// " test/* , docs/* " must be equivalent to "test/*,docs/*" after
	// leading/trailing whitespace is trimmed from each pattern.
	{
		FilterPolicy fp = makePolicyWithEnv(" test/* , docs/* ");
		CHECK(fp.shouldSkipEntry("test/foo.cpp", false));
		CHECK(fp.shouldSkipEntry("docs/bar.cpp", false));
		CHECK(!fp.shouldSkipEntry("src/main.cpp", false));
		printf("  [PASS] whitespace-trimmed patterns work\n");
	}

	// ── 3. Unset env var ──────────────────────────────────────────
	//
	// With the env var unset, loadExcludeEnv() returns false and no
	// user patterns are loaded. Built-in defaults still apply:
	//   - src/main.cpp → false (not a default skip)
	//   - test/foo.cpp → true (test is in default skip_dirs)
	{
		FilterPolicy fp = makePolicyWithEnv(nullptr);
		CHECK(!fp.shouldSkipEntry("src/main.cpp", false));
		CHECK(fp.shouldSkipEntry("test/foo.cpp",
					  false)); // default skip
		printf("  [PASS] unset env var: only defaults apply\n");
	}

	// ── 4. Directory skipping ─────────────────────────────────────
	//
	// "vendor/*" must skip the vendor/ directory entry itself.
	// Note: "vendor" is also in the built-in skip_dirs, so this is
	// true regardless — but we verify the glob also matches the dir.
	{
		FilterPolicy fp = makePolicyWithEnv("vendor/*");
		CHECK(fp.shouldSkipEntry("vendor/", true));
		// Also verify a non-default directory is skipped only via env:
		// "custom_dir/" is not in the built-in skip list.
		FilterPolicy fp2 = makePolicyWithEnv("custom_dir/*");
		CHECK(fp2.shouldSkipEntry("custom_dir/", true));
		// Without env var, custom_dir/ is NOT skipped.
		FilterPolicy fp3 = makePolicyWithEnv(nullptr);
		CHECK(!fp3.shouldSkipEntry("custom_dir/", true));
		printf("  [PASS] directory: vendor/ + custom_dir/ skipped via env\n");
	}

	// ── 5. Non-default path isolation ────────────────────────────
	//
	// "custom_skip/*" is not in the built-in skip_dirs, so it isolates
	// the env-var effect. With the env var set, custom_skip/foo.cpp is
	// skipped; without it, the same path is allowed.
	//
	// Also verifies glob semantics: "custom_skip/*" matches
	// "custom_skip/foo.cpp" but NOT "custom_skip/sub/bar.cpp" (the
	// single * does not cross path separators).
	{
		// With env var
		FilterPolicy fp = makePolicyWithEnv("custom_skip/*");
		CHECK(fp.shouldSkipEntry("custom_skip/foo.cpp", false));
		// * does not cross '/' — nested path NOT matched
		CHECK(!fp.shouldSkipEntry("custom_skip/sub/bar.cpp", false));

		// Without env var — same path is allowed
		FilterPolicy fp2 = makePolicyWithEnv(nullptr);
		CHECK(!fp2.shouldSkipEntry("custom_skip/foo.cpp", false));
		printf("  [PASS] non-default path: custom_skip/* skipped only with env\n");
	}

	// ── 6. Escaped comma inside a pattern ────────────────────────
	//
	// A directory whose name contains a literal comma ("a,b") yields the
	// pattern "a,b/**", which the producer escapes as "a\,b/**" so the comma
	// splitter does not tear it into "a\" + "b/**" (neither of which matches,
	// so the file would silently NOT be excluded).
	{
		FilterPolicy fp = makePolicyWithEnv("a\\,b/**,src/*");
		// The escaped comma belongs to ONE pattern, so the path matches.
		CHECK(fp.shouldSkipEntry("a,b/foo.cpp", false));
		// A neighbouring path is still not excluded.
		CHECK(!fp.shouldSkipEntry("a/other.cpp", false));
		// The unescaped comma still separates the second pattern.
		CHECK(fp.shouldSkipEntry("src/main.cpp", false));
		printf("  [PASS] escaped comma stays inside one pattern\n");
	}

	// Clean up: unset the env var so it doesn't leak to other tests.
	unsetenv("CODESCOPE_EXCLUDE_PATHS");

	printf("=== Exclude paths test passed ===\n");
	return checkFailures() ? 1 : 0;
}
