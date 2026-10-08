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
// Contract for a test's main(): the success path should end with
//
//     return checkFailures() ? 1 : 0;
//
// so that a recorded failure becomes a non-zero exit code. That part is now
// also ENFORCED rather than asked for: reportFailure() registers an exit hook
// that exits non-zero if any check failed, because 18 of the engine tests
// printed their banner and returned without consulting the counter — a failing
// CHECK there printed the failure, exited 0, and `make test-engine` (which
// judges by exit code) called it a pass.
//
// Usage:
//     #include "test_check.h"      // instead of <cassert>
//     ...
//     CHECK(pid > 0);              // no message
//     CHECK_MSG(rc == 0, "open");  // with a message
//     ...
//     return checkFailures() ? 1 : 0;   // belt and braces; the hook covers it

#include <cstdio>
#include <cstdlib>

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

/// Make a recorded failure outlive a main() that forgot to consult the
/// counter.
///
/// Registered by the first failure, which is also the first moment the answer
/// matters: a test that records none exits with whatever its main returns (0),
/// and a test that records one cannot exit 0 by accident. The hook runs at
/// exit and uses std::_Exit on purpose — calling exit() from an atexit handler
/// is undefined, and the remaining handlers could flush or print against state
/// that is already being torn down.
inline void failOnRecordedFailure()
{
	static const bool registered = []() {
		std::atexit([]() {
			if (failureCount() == 0)
				return;
			// Flush before _Exit: _Exit skips the remaining handlers on
			// purpose (calling exit() from a handler is undefined, and the
			// rest could print or flush against state already being torn
			// down), and under `make check` stdout is a fully buffered file,
			// so the failing test's own output — the diagnosis — would
			// otherwise be discarded exactly when it is needed.
			std::fflush(nullptr);
			std::_Exit(1);
		});
		return true;
	}();
	(void)registered;
}

/// Record a failed check: print `file:line`, the expression and an optional
/// message, then bump the counter.
inline void reportFailure(const char *expr, const char *file, int line,
			  const char *message)
{
	failOnRecordedFailure();
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
