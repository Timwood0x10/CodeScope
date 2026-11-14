#ifndef CODESCOPE_UTIL_TEST_CLASSIFIER_H
#define CODESCOPE_UTIL_TEST_CLASSIFIER_H

#include <string>

namespace util
{

/// Whether a file is test code, and why.
///
/// development_plan §12.2 (M1) asks for a production/test split before any
/// metric mixes them: a 120-line limit that counts test fixtures, or a
/// "function is unused" observation that ignores callers in tests, is wrong in a
/// way the caller cannot see. The classification is a documented heuristic, not
/// a truth — every answer carries the rule that produced it so a surprising
/// result can be argued with.
struct TestClassification {
	bool is_test = false;
	/// Machine-readable rule that decided it: "path:<segment>",
	/// "name:<pattern>", "content:<marker>", or "" for production code.
	std::string reason;
};

/// Classify one file.
///
/// Two rules, in order: a test directory segment (`tests/`, `spec/`,
/// `__tests__/`) and a test file name (`*_test.go`, `test_*.py`, `*.test.ts`,
/// `*_spec.rb`).
///
/// There is deliberately no content pass. A Rust file with an inline
/// `#[cfg(test)] mod tests` at the bottom is production code that happens to
/// contain tests: classifying the whole file as test would drop its production
/// symbols from every production metric — on this repository that misfiled 5 of
/// the 32 server modules. File-level classification answers "is this file's
/// purpose testing", which the path already says.
TestClassification classifyTestFile(const char *path);

} // namespace util

#endif // CODESCOPE_UTIL_TEST_CLASSIFIER_H
