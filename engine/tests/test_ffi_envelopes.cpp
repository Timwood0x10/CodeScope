// test_ffi_envelopes.cpp
//
// Smoke test for every FFI entry point that gained a try/catch wrapper (and
// for the two that only gained `catch (...)`). Two things are asserted:
//
//   1. Each entry point returns a heap-allocated JSON envelope — never null,
//      never a bare non-JSON string — when handed degenerate input (empty
//      strings, zero ids, a path that does not exist). Callers in the Rust
//      server feed the result straight into serde_json, so a non-JSON return
//      is a protocol break.
//   2. The process survives the whole sweep. That is the real point: these
//      exports previously had no try/catch, so any C++ exception crossing the
//      extern "C" boundary terminated the long-running MCP server. Reaching
//      the end of this test means no export threw an uncaught exception and
//      none of the wrappers was wired to the wrong implementation.
//
// It is deliberately NOT a behavioural test: the graph is empty, so most calls
// return an "empty"/"not ready" envelope. That is exactly what makes it stable.
//
// The exports are called with empty strings rather than nullptr: null-pointer
// validation is a separate contract (several of these already guard it, and a
// null dereference is a SIGSEGV that a try/catch cannot catch), so mixing the
// two concerns would make failures ambiguous.

#include "../include/engine.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/codescope_test_ffi_envelopes.db";
/// A path that does not exist, so indexing has nothing to walk.
static const char *kMissingDir = "/tmp/codescope_no_such_dir_for_ffi_smoke";

static int g_checked = 0;
static int g_failures = 0;

/// Assert an entry point returned a plausible JSON envelope, then release it.
static void expectEnvelope(const char *label, char *result)
{
	++g_checked;
	if (result == nullptr) {
		fprintf(stderr, "  FAIL %-38s null pointer\n", label);
		++g_failures;
		return;
	}
	const std::string json(result);
	engine_free_string(result);
	if (json.empty() || (json[0] != '{' && json[0] != '[')) {
		fprintf(stderr, "  FAIL %-38s not a JSON envelope: %.70s\n",
			label, json.c_str());
		++g_failures;
		return;
	}
	printf("  ok   %-38s\n", label);
}

int main()
{
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	if (engine_init(kDbPath) != 0) {
		fprintf(stderr, "FAIL: engine_init(%s)\n", kDbPath);
		return 1;
	}
	// Project 0 is intentionally invalid: every call below must still answer
	// with a JSON envelope rather than crash.
	const uint64_t pid = 0;

	printf("FFI entry points:\n");

	// ── Batch 2: the exports that gained a try/catch ────────────
	expectEnvelope("engine_index_project",
		       engine_index_project(pid, kMissingDir, ""));
	expectEnvelope("engine_index_files", engine_index_files(pid, "[]"));
	expectEnvelope("engine_scan_project",
		       engine_scan_project(pid, kMissingDir, ""));
	expectEnvelope("engine_get_module_tree", engine_get_module_tree(pid));
	expectEnvelope("engine_find_symbol", engine_find_symbol(pid, ""));
	expectEnvelope("engine_enhance_project", engine_enhance_project(pid));
	expectEnvelope("engine_get_enhancement_status",
		       engine_get_enhancement_status(pid));
	expectEnvelope("engine_unified_search",
		       engine_unified_search(pid, "", 10));
	expectEnvelope("engine_find_callers_adaptive",
		       engine_find_callers_adaptive(pid, "", ""));
	expectEnvelope("engine_find_callees_adaptive",
		       engine_find_callees_adaptive(pid, "", ""));
	expectEnvelope("engine_find_callers_by_entity",
		       engine_find_callers_by_entity(pid, 0));
	expectEnvelope("engine_find_callees_by_entity",
		       engine_find_callees_by_entity(pid, 0));
	expectEnvelope("engine_get_entry_points_new",
		       engine_get_entry_points_new(pid));
	expectEnvelope("engine_project_overview", engine_project_overview(pid));
	expectEnvelope("engine_trace_path", engine_trace_path(pid, "", ""));
	expectEnvelope("engine_explore_function",
		       engine_explore_function(pid, "", 1, "both"));
	expectEnvelope("engine_build_context", engine_build_context(pid, ""));
	expectEnvelope("engine_detect_ffi_boundaries",
		       engine_detect_ffi_boundaries(pid));
	expectEnvelope("engine_build_project_state",
		       engine_build_project_state(pid));
	expectEnvelope("engine_get_project_state",
		       engine_get_project_state(pid));
	expectEnvelope("engine_build_evidence", engine_build_evidence(pid, ""));
	// engine_verify_statement used to sit here; it was retired in favour of
	// verify_claim, so the envelope check now covers engine_verify_claim —
	// which this test did not cover at all before.
	expectEnvelope("engine_verify_claim", engine_verify_claim(pid, ""));

	// ── The two that gained `catch (...)` ──────────────────────
	expectEnvelope("engine_search_semantic",
		       engine_search_semantic(pid, "", 10));
	expectEnvelope("engine_rebuild_csr", engine_rebuild_csr("", pid));

	engine_shutdown();
	unlink(kDbPath);
	unlink((std::string(kDbPath) + "-wal").c_str());
	unlink((std::string(kDbPath) + "-shm").c_str());

	printf("\nchecked %d entry points, %d failure(s)\n", g_checked,
	       g_failures);
	if (g_failures != 0)
		return 1;
	printf("\n=== test_ffi_envelopes PASSED ===\n");
	printf("Every patched FFI entry point returned a JSON envelope and the "
	       "process survived\n");
	return 0;
}
