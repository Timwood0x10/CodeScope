// test_gitignore_build_dirs.cpp — regression test for `.gitignore` directory
// pruning, written after a real-project index was found to contain entities and
// modules from build directories.
//
// Observed on this repository (fresh self-index, 2026-09-21):
//   * 14 entities whose file_path is inside `build-cold` / `build-verify` /
//     `build-release`(x2 CMake versions) / `build-tests`(x2) /
//     `build-release-linux`, all of them CMake's compiler-probe files
//     (`CMakeFiles/<ver>/CompilerIdC*/CMakeCCompilerId.c` and `…CXX.cpp`);
//   * 7 of the 43 rows in the `modules` table are those `…/CMakeFiles/<ver>`
//     directories — i.e. the leak is user-visible through get_module_tree.
//
// `engine/build/` contributes zero, and the difference between that and
// `engine/build-cold/` is exactly what this file pins down: `build` is in
// FilterPolicy's hard-skip list while `build-*` is not, and the repository's
// `.gitignore` — which does list `**/build-*/` — is what is supposed to cover
// the rest.
//
// Uses the explicit check() pattern: assert() is compiled out under NDEBUG,
// which would make these checks vacuous in a Release build.
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../src/filter_policy.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static int g_failed = 0;

static void check(bool cond, const std::string &what)
{
	if (cond) {
		printf("  [PASS] %s\n", what.c_str());
		return;
	}
	fprintf(stderr, "  [FAIL] %s\n", what.c_str());
	g_failed++;
}

static void expectSkip(const FilterPolicy &p, const std::string &rel,
		       bool is_dir, bool want_skip, const char *why)
{
	bool got = p.shouldSkipEntry(rel, is_dir);
	bool ok = (got == want_skip);
	printf("  [%s] shouldSkipEntry(\"%s\", is_dir=%d) = %s, want %s (%s)\n",
	       ok ? "PASS" : "FAIL", rel.c_str(), is_dir ? 1 : 0,
	       got ? "SKIP" : "INDEX", want_skip ? "SKIP" : "INDEX", why);
	if (!ok)
		g_failed++;
}

int main()
{
	std::error_code ec;
	auto root = std::filesystem::temp_directory_path(ec) /
		    "codescope_gitignore_build_dirs";
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);
	if (ec) {
		fprintf(stderr, "FAIL: cannot create fixture %s: %s\n",
			root.c_str(), ec.message().c_str());
		return 1;
	}

	// The fixture mirrors this repository's shape: a .gitignore that ignores
	// build trees by pattern, a source tree next to them, and the CMake
	// compiler-probe files that a build directory legitimately contains.
	const char *gitignore = "# fixture\n"
				"**/build/\n"
				"**/build-*/\n"
				"node_modules/\n"
				"*.o\n";
	{
		std::ofstream f(root / ".gitignore");
		f << gitignore;
	}
	const char *probe = "CMakeFiles/4.4.3/CompilerIdC/CMakeCCompilerId.c";
	std::filesystem::create_directories(
		root / "engine" / "build-cold" /
			std::filesystem::path(probe).parent_path(),
		ec);
	std::filesystem::create_directories(root / "engine" / "src", ec);
	{
		std::ofstream f(root / "engine" / "build-cold" / probe);
		f << "int main(void){return 0;}\n";
		std::ofstream g(root / "engine" / "src" / "app.cpp");
		g << "int main(){return 0;}\n";
	}

	FilterPolicy p;
	bool loaded = p.loadGitignore(root.string());
	printf("=== .gitignore build-directory pruning ===\n");
	check(loaded, "fixture .gitignore loaded");

	// The directory itself — this is the case that decides whether the walker
	// ever descends into the tree at all.
	expectSkip(p, "engine/build-cold", true, true,
		   "`**/build-*/` names the directory");
	expectSkip(p, "engine/build", true, true,
		   "`**/build/` (also hard-skipped)");
	expectSkip(p, "engine/build-release-linux", true, true,
		   "second build-* directory");

	// The files inside it. Per gitignore semantics an ignored directory
	// ignores everything under it, so these must be skipped even though the
	// pattern ends at the directory name.
	expectSkip(p, "engine/build-cold/" + std::string(probe), false, true,
		   "file inside an ignored directory");
	expectSkip(p, "engine/build-release-linux/" + std::string(probe), false,
		   true, "file inside a second ignored directory");

	// Controls: the fix must not start skipping ordinary source.
	expectSkip(p, "engine/src/app.cpp", false, false, "first-party source");
	expectSkip(p, "engine/src", true, false, "source directory");

	// The parallel scheduler's case: a per-module worker scans
	// `<root>/<module>` and hands the policy MODULE-relative paths, while the
	// rules it loaded are anchored at the project root. The prefix puts the
	// module component back, which is what lets a rule ending at the module
	// name exclude the module's whole content. Without it, the module's own
	// files were indexed (observed as a `build-x` module in the merged graph).
	FilterPolicy mp;
	check(mp.loadGitignore(root.string()),
	      "module worker: .gitignore loaded");
	mp.setScanPrefix("engine/");
	expectSkip(mp, "build-cold/" + std::string(probe), false, true,
		   "module-relative path, project-root rule");
	expectSkip(mp, "build-release-linux", true, true,
		   "module-relative directory");
	expectSkip(mp, "src/app.cpp", false, false,
		   "module-relative first-party source");

	printf("=== %s (%d failure%s) ===\n", g_failed ? "FAILED" : "PASSED",
	       g_failed, g_failed == 1 ? "" : "s");
	std::filesystem::remove_all(root, ec);
	return g_failed ? 1 : 0;
}
