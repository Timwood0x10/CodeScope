// test_ir_deep_nesting.cpp — regression test for the visitor recursion depth
// guard (Bug: unbounded AST recursion → SIGSEGV).
//
// Before the fix:
//   JsVisitor::visitChildren (the single choke point every language visitor
//   descends through) had no depth limit. tree-sitter happily parses a
//   pathologically deep AST — e.g. ~20k nested call expressions — and the
//   recursive walk then overflowed the native C++ stack and crashed with
//   SIGSEGV. The FFI try/catch boundary cannot recover a stack overflow, so
//   indexing an untrusted/generated file could kill the worker.
//
// After the fix:
//   visitChildren stops descending past kMaxVisitDepth (512) and reports the
//   truncation once on stderr. Indexing completes with "ok":true, the file's
//   top-level symbols are still recorded, and the process never crashes.
//
// This test builds a deeply nested source file and asserts the engine returns
// a normal success envelope instead of crashing.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

static inline void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

// Depth chosen well above kMaxVisitDepth (512) and into the range that
// reproduced the SIGSEGV in the original report (~20k). If the guard were
// removed this test would crash rather than fail an assertion.
static constexpr int kDefaultNestingDepth = 20000;

static int nestingDepth()
{
	// Overridable for local bisection while debugging the guard.
	if (const char *env = getenv("IR_DEEP_NESTING_DEPTH")) {
		int v = atoi(env);
		if (v > 0)
			return v;
	}
	return kDefaultNestingDepth;
}

// Build `f(f(f(...f(0)...)))` with kNestingDepth nested call expressions,
// assigned inside a top-level function so a shallow symbol also exists.
static std::string makeDeeplyNestedPython()
{
	const int depth = nestingDepth();
	std::string body;
	body.reserve(static_cast<size_t>(depth) * 3 + 64);
	for (int i = 0; i < depth; i++)
		body += "f(";
	body += "0";
	for (int i = 0; i < depth; i++)
		body += ")";

	std::string src = "def f(x):\n    return x\n\n";
	src += "def top():\n    return ";
	src += body;
	src += "\n";
	return src;
}

int main()
{
	const char *proj_dir = "/tmp/ir_deep_nesting_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	const std::string py_path = std::string(proj_dir) + "/deep.py";
	FILE *f = fopen(py_path.c_str(), "w");
	check(f != nullptr, "fopen deep.py");
	const std::string src = makeDeeplyNestedPython();
	check(fwrite(src.data(), 1, src.size(), f) == src.size(),
	      "write deep.py");
	fclose(f);

	char db[] = "/tmp/test_ir_deep_nesting.db";
	unlink(db);
	unlink("/tmp/test_ir_deep_nesting.db-wal");
	unlink("/tmp/test_ir_deep_nesting.db-shm");

	check(engine_init(db) == 0, "engine_init");

	uint64_t pid = engine_create_project(proj_dir, "deep-nesting");
	check(pid > 0, "create_project");

	// The assertion that matters most is implicit: this call must RETURN.
	// Without the depth guard the process SIGSEGVs here and the test binary
	// exits with a signal — a failure under `make test-engine`.
	char *idx = engine_index_project(pid, proj_dir, nullptr);
	check(idx != nullptr, "index_project returns non-null (no crash)");
	check(strstr(idx, "\"ok\":true") != nullptr,
	      "index_project reports ok:true on deeply nested input");
	engine_free_string(idx);

	// Indexing still finished: the top-level function survived the walk
	// (it is emitted before the deep subtree is truncated).
	char *sym = engine_find_symbol(pid, "top");
	check(sym != nullptr, "find_symbol(top) non-null");
	check(strstr(sym, "top") != nullptr,
	      "top-level function must still be indexed after truncation");
	engine_free_string(sym);

	// Join the async knowledge-builder thread before returning — a
	// std::thread destroyed while joinable calls std::terminate.
	engine_shutdown();
	std::filesystem::remove_all(proj_dir);

	fprintf(stderr, "PASS: deep-nesting guard held (depth=%d)\n",
		nestingDepth());
	return 0;
}
