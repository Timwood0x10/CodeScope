// test_filter_policy_depth.cpp — regression test for dir-skip depth policy.
//
// Defect guarded: `scripts`, `hack`, `migrations`, `seeds`, `integration`,
// `locale(s)`, `i18n`, `l10n`, `assets`, `static`, `public`, `media` and
// `external` were skipped at ANY path depth. Those names are ordinary
// business directories outside the web-frontend convention
// (src/integration/, pkg/scripts/, db/migrations/, app/static/), so the
// skip silently dropped first-party source files — symbols and call edges
// the index then reported as non-existent, i.e. an undetectable false
// negative. The non-source junk they were meant to exclude (png/po/json/
// css) is already rejected by the source-extension filter.
//
// Policy now enforced: those names are skipped only within the FIRST THREE
// path components (the depth at which the web conventions actually live),
// for every language; the universally non-source / vendor names (tests,
// docs, vendor, node_modules, ...) stay any-depth.
//
// The Java carve-out is unchanged and re-checked here: Java package
// namespaces still tolerate the any-depth names beyond the top 3 levels.
//
// Uses the explicit check() pattern: assert() is compiled out under
// NDEBUG, which would make these checks vacuous in a Release build.
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../src/filter_policy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

static void expectPath(const FilterPolicy &p, const std::string &rel_path,
		       bool want_skip, const char *why)
{
	bool got = p.shouldSkipPath(rel_path, false);
	if (got != want_skip) {
		fprintf(stderr,
			"FAIL: shouldSkipPath(\"%s\") = %s, expected %s "
			"(%s)\n",
			rel_path.c_str(), got ? "SKIP" : "INDEX",
			want_skip ? "SKIP" : "INDEX", why);
		exit(1);
	}
	printf("  [PASS] %-45s %s (%s)\n", rel_path.c_str(),
	       got ? "SKIP" : "INDEX", why);
}

int main()
{
	FilterPolicy p; // NORMAL mode, no ignore files loaded

	printf("=== filter policy depth regression tests (NORMAL mode) ===\n");

	// ── Business-plausible names: skipped at depth 1-3 only ────────
	expectPath(p, "public/index.js", true,
		   "root-level public/ is the web convention (depth 1)");
	expectPath(p, "web/public/app.js", true, "depth 2 still skipped");
	expectPath(p, "src/app/public/widget.js", true,
		   "depth 3 still skipped");
	expectPath(p, "src/features/deep/public/widget.js", false,
		   "depth 4 is first-party code — must be indexed");
	expectPath(p, "src/pkg/scripts/main.py", true,
		   "depth 3 still skipped (boundary)");
	expectPath(p, "src/pkg/tools/scripts/main.py", false,
		   "deep scripts/ is real code — must be indexed");
	expectPath(p, "src/a/b/integration/real.go", false,
		   "deep integration/ is real code — must be indexed");
	expectPath(p, "src/x/y/z/migrations/0001.sql.go", false,
		   "deep migrations/ is real code — must be indexed");
	expectPath(p, "migrations/0001_init.sql", true,
		   "root migrations/ still skipped (depth 1)");

	// ── Universally non-source / vendor names: still any-depth ─────
	expectPath(p, "src/deep/nested/tests/x_test.go", true,
		   "tests stays any-depth (documented policy)");
	expectPath(p, "src/deep/nested/vendor/lib.go", true,
		   "vendor stays any-depth");
	expectPath(p, "packages/app/node_modules/x/index.js", true,
		   "node_modules stays any-depth");
	expectPath(p, "src/deep/third_party/util.c", true,
		   "third_party stays any-depth");

	// ── Extension filter still rejects the asset junk ───────────────
	// This is what made the depth relaxation safe: the formats that fill
	// assets//media//public// never reach the parser anyway.
	check(p.shouldSkipSuffix(".png"),
	      ".png must still be rejected by the extension filter");
	check(p.shouldSkipSuffix(".class"),
	      ".class must still be rejected by the extension filter");
	check(!p.shouldSkipSuffix(".go"), ".go must not be rejected");
	expectPath(p, "src/features/deep/assets/real.py", false,
		   "deep assets/ holding real code must be indexed");
	printf("  [PASS] extension filter still rejects asset formats\n");

	// ── Java carve-out still protects package namespaces ────────────
	FilterPolicy java;
	java.setLangContext("java");
	// samples at depth 4 is an org/... package component, not a docs
	// folder: the Java carve-out must keep it indexed.
	expectPath(java, "org/acme/store/samples/Widget.java", false,
		   "Java: samples beyond top 3 is a package component");
	// ... while the top-3 levels are still skipped for Java.
	expectPath(java, "src/test/java/App.java", true,
		   "Java: src/test/ (depth 2) still skipped");
	expectPath(java, "org/acme/store/deep/public/App.java", false,
		   "Java: deep public/ is a package component too");

	printf("\nAll filter policy depth tests passed.\n");
	return 0;
}
