// test_ir_deep_nesting.cpp — regression test for the visitor recursion depth
// guards (bug: unbounded AST recursion → SIGSEGV) and for their truncation
// contract.
//
// Before the fix:
//   the recursive walks had no depth limit. tree-sitter happily parses a
//   pathologically deep AST — e.g. ~20k nested call expressions — and a walk
//   then overflowed the native C++ stack and crashed with SIGSEGV. The FFI
//   try/catch boundary cannot recover a stack overflow, so indexing an
//   untrusted/generated file could kill the worker.
//
// After the fix:
//   every recursive walk is bounded by `kMaxVisitDepth`: the traversal
//   (`JsVisitor::visitChild`, the single guarded descent every recursive step
//   goes through, including language handlers that recurse on their own), the
//   defined-names pre-pass (`JsVisitor::collectDefinedNames`) and the C++
//   out-of-class-definition pre-scan (`CVisitor::collectOutOfClassDefs`). The
//   first walk to reach the limit reports the file once on stderr, tagged
//   [module=ir, method=…]; the others stay quiet for that file. Indexing
//   completes with "ok":true and the file's top-level symbols are recorded.
//
// This test asserts both halves of that contract: a file well below the cap is
// indexed with no truncation report, and 20k-deep files (Python calls, Python
// parens, C++ parens) each survive and produce exactly one report. Without the
// guards the deep cases crash the process instead of failing an assertion.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include "test_engine_handle.h"

static inline void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

/// Depth well below kMaxVisitDepth: nothing may be dropped.
static constexpr int kShallowNestingDepth = 100;

/// Depth chosen far above kMaxVisitDepth and into the range that reproduced
/// the SIGSEGV in the original report (~20k). If the guards were removed this
/// test would crash rather than fail an assertion.
static constexpr int kDefaultNestingDepth = 20000;

/// The marker every depth guard prints when it truncates.
static const char *kTruncationMarker = "exceeded kMaxVisitDepth";

static int nestingDepth()
{
	// Overridable for local bisection while debugging the guards.
	if (const char *env = getenv("IR_DEEP_NESTING_DEPTH")) {
		int v = atoi(env);
		if (v > 0)
			return v;
	}
	return kDefaultNestingDepth;
}

/// Shape of the generated Python expression.
enum class DeepShape {
	/// `f(f(f(...f(0)...)))` — the shape from the original crash report. The
	/// call handler consumes each argument itself, so this nests deeply in the
	/// DEFINED-NAMES pre-pass (which walks every node) but not in the main
	/// traversal.
	NestedCalls,
	/// `((((...0...))))` — plain expression nesting, which both walks descend
	/// node by node.
	NestedParens,
};

// Build a Python file whose `top()` returns `depth`-nested expressions, with a
// shallow top-level function so a symbol exists outside the deep subtree.
static std::string makeDeeplyNestedPython(int depth, DeepShape shape)
{
	std::string body;
	body.reserve(static_cast<size_t>(depth) * 4 + 64);
	if (shape == DeepShape::NestedCalls) {
		for (int i = 0; i < depth; i++)
			body += "f(";
		body += "0";
		for (int i = 0; i < depth; i++)
			body += ")";
	} else {
		body.append(static_cast<size_t>(depth), '(');
		body += "0";
		body.append(static_cast<size_t>(depth), ')');
	}

	std::string src = "def f(x):\n    return x\n\n";
	src += "def top():\n    return ";
	src += body;
	src += "\n";
	return src;
}

// Build a C++ file with the same plain expression nesting. Exercises the C++
// pre-scans as well as the traversal.
static std::string makeDeeplyNestedCpp(int depth)
{
	std::string body;
	body.reserve(static_cast<size_t>(depth) * 2 + 64);
	body.append(static_cast<size_t>(depth), '(');
	body += "0";
	body.append(static_cast<size_t>(depth), ')');

	std::string src = "int top() { return ";
	src += body;
	src += "; }\n";
	return src;
}

/// Index one generated file into a fresh database and return everything the
/// engine wrote to stderr during the run.
///
/// stderr (not the JSON result) is the observable that proves a guard engaged:
/// the truncation report is a one-time diagnostic, not part of the tool
/// response. The descriptor is restored before returning so a failing
/// assertion below is still visible on the terminal.
///
/// \param source     File contents to index.
/// \param file_name  Name to write it under (extension selects the language).
/// \param proj_dir   Scratch project directory (recreated).
/// \param db_path    Database path (recreated).
/// \return           Captured stderr of the whole index run.
static std::string indexCase(const std::string &source,
			     const std::string &file_name,
			     const std::string &proj_dir,
			     const std::string &db_path)
{
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir);

	const std::string src_path = proj_dir + "/" + file_name;
	FILE *f = fopen(src_path.c_str(), "w");
	check(f != nullptr, "fopen source file");
	check(fwrite(source.data(), 1, source.size(), f) == source.size(),
	      "write source file");
	fclose(f);

	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());

	const std::string capture = db_path + ".stderr";
	fflush(stderr);
	const int saved_fd = dup(fileno(stderr));
	check(saved_fd >= 0, "dup(stderr)");
	check(freopen(capture.c_str(), "w", stderr) != nullptr,
	      "freopen stderr to capture file");

	g_engine = engine_create(db_path.c_str());
	check(g_engine != nullptr, "engine_init");
	uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
					     "deep-nesting");
	check(pid > 0, "create_project");

	// The assertion that matters most is implicit: this call must RETURN.
	// Without the guards the process SIGSEGVs here and the test binary exits
	// with a signal — a failure under `make test-engine`.
	char *idx =
		engine_index_project(g_engine, pid, proj_dir.c_str(), nullptr);
	check(idx != nullptr, "index_project returns non-null (no crash)");
	check(strstr(idx, "\"ok\":true") != nullptr,
	      "index_project reports ok:true on deeply nested input");
	engine_free_string(idx);

	// Indexing still finished: the top-level function survived the walk (it is
	// emitted before the deep subtree is truncated).
	char *sym = engine_find_symbol(g_engine, pid, "top");
	check(sym != nullptr, "find_symbol(top) non-null");
	check(strstr(sym, "top") != nullptr,
	      "top-level function must still be indexed after truncation");
	engine_free_string(sym);

	// Join the async knowledge-builder thread before returning — a
	// std::thread destroyed while joinable calls std::terminate.
	engine_destroy(g_engine);
	g_engine = nullptr;

	fflush(stderr);
	dup2(saved_fd, fileno(stderr));
	close(saved_fd);

	std::string captured;
	if (FILE *cap = fopen(capture.c_str(), "r")) {
		char buf[4096];
		size_t n = 0;
		while ((n = fread(buf, 1, sizeof buf, cap)) > 0)
			captured.append(buf, n);
		fclose(cap);
	}
	return captured;
}

/// Count non-overlapping occurrences of `needle` in `haystack`.
static int countOccurrences(const std::string &haystack, const char *needle)
{
	int n = 0;
	for (size_t pos = haystack.find(needle); pos != std::string::npos;
	     pos = haystack.find(needle, pos + 1))
		n++;
	return n;
}

int main()
{
	const int deep = nestingDepth();

	// ── 1. Below the cap: no truncation, nothing dropped ─────────
	const std::string shallow =
		indexCase(makeDeeplyNestedPython(kShallowNestingDepth,
						 DeepShape::NestedCalls),
			  "deep.py", "/tmp/ir_deep_nesting_shallow",
			  "/tmp/test_ir_shallow_nesting.db");
	check(countOccurrences(shallow, kTruncationMarker) == 0,
	      "a file below kMaxVisitDepth must not be truncated");

	// ── 2. Deeply nested Python CALLS (the original crash) ───────
	const std::string calls_err = indexCase(
		makeDeeplyNestedPython(deep, DeepShape::NestedCalls), "deep.py",
		"/tmp/ir_deep_nesting_repro", "/tmp/test_ir_deep_nesting.db");
	check(countOccurrences(calls_err, kTruncationMarker) == 1,
	      "a file far above kMaxVisitDepth must report the truncation once");

	// ── 3. Deeply nested Python PARENS ───────────────────────────
	const std::string parens_err =
		indexCase(makeDeeplyNestedPython(deep, DeepShape::NestedParens),
			  "deep.py", "/tmp/ir_deep_nesting_parens",
			  "/tmp/test_ir_deep_nesting_parens.db");
	check(countOccurrences(parens_err, kTruncationMarker) == 1,
	      "deep expression nesting must be bounded and reported once");
	// The report is shared per file (JsVisitor::depth_truncated_): whichever
	// capped walk reaches the limit first reports the file and the others stay
	// quiet for it, so a deep file yields exactly one diagnostic — never one
	// per node, and never none.
	check(parens_err.find("[module=ir, method=") != std::string::npos,
	      "the truncation report must carry its module/method tag");

	// ── 4. Deeply nested C++ PARENS ──────────────────────────────
	// Same shape through the C visitor: this exercises the C++ pre-scans
	// (collectDefinedNames + collectOutOfClassDefs) in addition to the
	// traversal, all of which recurse over the tree.
	const std::string cpp_err = indexCase(
		makeDeeplyNestedCpp(deep), "deep.cpp",
		"/tmp/ir_deep_nesting_cpp", "/tmp/test_ir_deep_nesting_cpp.db");
	check(countOccurrences(cpp_err, kTruncationMarker) == 1,
	      "a deep C++ file must be bounded and reported once");

	fprintf(stderr,
		"PASS: deep-nesting guards held (shallow=%d, deep=%d, "
		"py-calls/py-parens/cpp-parens)\n",
		kShallowNestingDepth, deep);
	return 0;
}
