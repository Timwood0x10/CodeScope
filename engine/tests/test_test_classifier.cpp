// test_test_classifier.cpp — production/test classification (§12.2).
//
// Every metric that counts test code as production is wrong invisibly, so the
// rules are pinned here: the directory, the file name and the inline marker, and
// the fact that a production file is NOT called a test just because a substring
// like "latest" appears in its path.

#include "../src/util/test_classifier.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void expectTest(const std::string &path, const char *why)
{
	const auto c = util::classifyTestFile(path.c_str());
	if (!c.is_test) {
		std::fprintf(stderr, "FAIL: %s should be a test file (%s)\n",
			     path.c_str(), why);
		std::exit(1);
	}
	if (c.reason.empty()) {
		std::fprintf(stderr, "FAIL: %s has no reason\n", path.c_str());
		std::exit(1);
	}
}

static void expectProduction(const std::string &path)
{
	const auto c = util::classifyTestFile(path.c_str());
	if (c.is_test) {
		std::fprintf(stderr, "FAIL: %s classified as test (%s)\n",
			     path.c_str(), c.reason.c_str());
		std::exit(1);
	}
}

int main()
{
	// Directories.
	expectTest("/p/tests/foo.cpp", "tests/ segment");
	expectTest("engine/tests/test_loc_counter.cpp", "tests/ segment");
	expectTest("src/spec/handler.rb", "spec/ segment");
	expectTest("web/__tests__/a.js", "__tests__");
	// File names.
	expectTest("src/store_test.go", "*_test.go");
	expectTest("pkg/test_parser.py", "test_*.py");
	// A C++ header that merely talks about tests is production code.
	expectProduction("engine/src/util/test_classifier.h");
	expectTest("web/app.test.ts", "*.test.ts");
	expectTest("web/app.spec.tsx", "*.spec.tsx");
	expectTest("engine/tests/store_core.cpp", "tests/");
	expectTest("src/graph_test.c", "*_test.c");
	// A production file that contains an inline test module stays production:
	// its symbols are production symbols, and misfiling the file would remove
	// them from every production metric.
	expectProduction("server/src/scheduler/mod.rs");
	expectProduction("engine/src/gtest_support.cpp");

	// Production code must not be dragged in by a substring.
	expectProduction("engine/src/latest/loader.cpp");
	expectProduction("engine/src/store/store_core.cpp");
	expectProduction("server/src/tools/catalog.rs");
	// "contest/" contains "test" but is not a test directory.
	expectProduction("app/contest/entry.cpp");

	std::printf("test_test_classifier: all tests passed\n");
	return 0;
}
