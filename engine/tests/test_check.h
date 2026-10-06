#ifndef CODESCOPE_TEST_CHECK_H
#define CODESCOPE_TEST_CHECK_H

// Shared non-vacuous check macro for the C++ engine tests.
//
// Why not <cassert>: assert() is compiled out when NDEBUG is defined — i.e.
// in every Release / RelWithDebInfo build — so an assert-based test silently
// becomes a no-op that still "passes". That is the extreme form of the
// under-testing REVIEW_0.2.7.md TEST-3 describes, and it once masked a CI
// skip-list drift where 26 tests never really ran.
//
// CHECK() is always evaluated, and on failure it prints the source location,
// the expression and an optional message. Unlike assert() it does NOT abort:
// it records the failure and lets the test continue, so one broken expectation
// cannot hide the ones after it.
//
// Contract for a test's main(): the success path must end with
//
//     return checkFailures() ? 1 : 0;
//
// so that a recorded failure becomes a non-zero exit code. Without that, the
// failure is only printed and the test still exits 0 — which `make test-engine`
// would read as a pass.
//
// Usage:
//     #include "test_check.h"      // instead of <cassert>
//     ...
//     CHECK(pid > 0);              // no message
//     CHECK_MSG(rc == 0, "open");  // with a message
//     ...
//     return checkFailures() ? 1 : 0;

#include <cstdio>

namespace codescope_test
{

/// Failure counter for the current test binary.
///
/// The function-local static lives in an inline function, so every translation
/// unit in one test binary shares the same counter.
inline int &failureCount()
{
	static int count = 0;
	return count;
}

/// Record a failed check: print `file:line`, the expression and an optional
/// message, then bump the counter.
inline void reportFailure(const char *expr, const char *file, int line,
			  const char *message)
{
	++failureCount();
	if (message && *message)
		std::fprintf(stderr, "CHECK FAILED: %s (%s) at %s:%d\n", expr,
			     message, file, line);
	else
		std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, file,
			     line);
}

} // namespace codescope_test

/// Evaluate `cond`; on failure record it and continue.
#define CHECK(cond)                                                         \
	do {                                                                \
		if (!(cond))                                                \
			::codescope_test::reportFailure(#cond, __FILE__,    \
							__LINE__, nullptr); \
	} while (0)

/// CHECK() with a caller-supplied explanation.
#define CHECK_MSG(cond, message)                                              \
	do {                                                                  \
		if (!(cond))                                                  \
			::codescope_test::reportFailure(#cond, __FILE__,      \
							__LINE__, (message)); \
	} while (0)

/// Number of failed checks recorded so far (0 == success).
inline int checkFailures()
{
	return codescope_test::failureCount();
}

#endif // CODESCOPE_TEST_CHECK_H
