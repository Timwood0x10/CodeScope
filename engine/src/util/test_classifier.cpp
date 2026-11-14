// test_classifier.cpp — production vs test classification (development_plan
// §12.2, §16.11).
//
// The rules are deliberately small and ordered, because the answer is fed into
// metrics: `function_too_long` on a 40-line fixture is noise, and "no callers"
// on a helper whose only callers are tests is a lie. Each rule reports its own
// name so a classification that surprises someone can be traced to one line
// instead of "the heuristic".

#include "test_classifier.h"

#include <algorithm>
#include <string>
#include <vector>

namespace util
{
namespace
{

/// Directory names that mean "this subtree is tests" in the ecosystems the
/// engine parses. Matched as whole path segments, so `latest/` is not `test/`.
constexpr const char *kTestDirs[] = {
	"test",	      "tests",	  "spec",     "specs", "__tests__",
	"__mocks__",  "testdata", "fixtures", "e2e",   "integration_tests",
	"unit_tests",
};

/// Suffix / prefix rules, checked on the file name only.
struct NameRule {
	const char *suffix; // matched case-insensitively at the end
	const char *prefix; // matched case-insensitively at the start
	const char *reason;
};

constexpr NameRule kNameRules[] = {
	{ "_test.go", nullptr, "name:*_test.go" },
	{ "_test.py", nullptr, "name:*_test.py" },
	{ "_test.rs", nullptr, "name:*_test.rs" },
	{ ".test.js", nullptr, "name:*.test.js" },
	{ ".test.ts", nullptr, "name:*.test.ts" },
	{ ".test.tsx", nullptr, "name:*.test.tsx" },
	{ ".spec.js", nullptr, "name:*.spec.js" },
	{ ".spec.ts", nullptr, "name:*.spec.ts" },
	{ ".test.jsx", nullptr, "name:*.test.jsx" },
	{ ".spec.jsx", nullptr, "name:*.spec.jsx" },
	{ ".spec.tsx", nullptr, "name:*.spec.tsx" },
	{ "_spec.rb", nullptr, "name:*_spec.rb" },
	{ "_test.c", nullptr, "name:*_test.c" },
	{ "_test.cpp", nullptr, "name:*_test.cpp" },
	{ "_test.cc", nullptr, "name:*_test.cc" },
};

std::string lower(const std::string &s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
		       [](unsigned char c) {
			       return static_cast<char>(std::tolower(c));
		       });
	return out;
}

/// Every '/' separated segment of `path`, lowercased.
std::vector<std::string> segments(const std::string &path)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start <= path.size()) {
		const std::size_t slash = path.find('/', start);
		const std::size_t end =
			slash == std::string::npos ? path.size() : slash;
		if (end > start)
			out.push_back(lower(path.substr(start, end - start)));
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return out;
}

std::string fileName(const std::string &path)
{
	const std::size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

TestClassification classifyTestFile(const char *path)
{
	TestClassification out;
	if (!path || !*path)
		return out;
	const std::string file(path);

	// 1. A test directory anywhere in the path. Checked on segments so
	//    "latest/" does not match "test".
	const std::vector<std::string> parts = segments(file);
	for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
		for (const char *dir : kTestDirs) {
			if (parts[i] == dir) {
				out.is_test = true;
				out.reason = std::string("path:") + dir + "/";
				return out;
			}
		}
	}

	// 2. A test file name. `test_*` is a Python convention, so it is scoped to
	//    .py: a C++ header named test_classifier.h is production code that
	//    talks about tests, and calling it a test file removed it (and its
	//    symbols) from production metrics.
	const std::string name = lower(fileName(file));
	const bool is_python = name.size() > 3 &&
			       name.compare(name.size() - 3, 3, ".py") == 0;
	if (is_python && name.rfind("test_", 0) == 0) {
		out.is_test = true;
		out.reason = "name:test_*.py";
		return out;
	}
	for (const auto &rule : kNameRules) {
		if (rule.suffix &&
		    name.size() >= std::string(rule.suffix).size() &&
		    name.compare(name.size() - std::string(rule.suffix).size(),
				 std::string(rule.suffix).size(),
				 rule.suffix) == 0) {
			out.is_test = true;
			out.reason = rule.reason;
			return out;
		}
		if (rule.prefix && name.rfind(rule.prefix, 0) == 0) {
			out.is_test = true;
			out.reason = rule.reason;
			return out;
		}
	}

	out.reason = "";
	return out;
}

} // namespace util
