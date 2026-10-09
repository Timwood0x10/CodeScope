#ifndef CODESCOPE_TESTS_GRAMMAR_LOADER_H
#define CODESCOPE_TESTS_GRAMMAR_LOADER_H

// The grammar a test parses with, taken from the library the test already links.
//
// engine/src/codescope_grammars.h states the rule outright — "These are compiled
// into the binary — no dlopen, no .so loading" — and every test binary links
// astgraph_engine, so the language is one call away. Five tests instead dlopen()ed
// `<dir>/tree-sitter-<grammar>.so` over a candidate list of `$GRAMMARS_DIR`,
// `../grammars`, `grammars`, which does not contain `engine/grammars` at all
// (three of them ended the list with a hard-coded personal path behind a
// full-width "～"). No target in this repository produces those files —
// engine/grammars/*.so are untracked leftovers on a developer machine — so the
// tests could only pass where the artefacts happened to sit next to a matching
// working directory. On CI, which runs `engine/build-tests/test_*` from the
// repository root with GRAMMARS_DIR unset, they failed on `grammar loaded`:
// test_js_visitor, test_ts_visitor, test_tsx_visitor and, through the same
// loader, test_translator_depth_guard and test_ir_edge_coverage. Asking the
// library removes the working directory, the environment and the stale
// artefact from the answer.

#include "../src/codescope_grammars.h"

#include <cstring>
#include <tree_sitter/api.h>

/// The statically linked grammar called `name` ("c", "cpp", "go", "java",
/// "javascript", "python", "rust", "typescript", "tsx"), or nullptr when the
/// name is not one of the vendored grammars. A nullptr is a test bug — the
/// table below is the whole set — so callers keep a CHECK on the result.
inline const TSLanguage *testGrammar(const char *name)
{
	struct Entry {
		const char *name;
		const TSLanguage *(*language)();
	};
	static const Entry kGrammars[] = {
		{ "c", tree_sitter_c },
		{ "cpp", tree_sitter_cpp },
		{ "go", tree_sitter_go },
		{ "java", tree_sitter_java },
		{ "javascript", tree_sitter_javascript },
		{ "python", tree_sitter_python },
		{ "rust", tree_sitter_rust },
		{ "typescript", tree_sitter_typescript },
		{ "tsx", tree_sitter_tsx },
	};
	for (const Entry &entry : kGrammars) {
		if (std::strcmp(entry.name, name) == 0)
			return entry.language();
	}
	return nullptr;
}

#endif // CODESCOPE_TESTS_GRAMMAR_LOADER_H
