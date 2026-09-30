/**
 * Unit test for the LEGACY-translator recursion bound
 * (CODE_REVIEW_2026-09-27.md, fourth round item F1).
 *
 * The LIVE IR pipeline is the visitors, which bound their recursion with
 * kMaxVisitDepth and report the truncation. The translators (`*_translator.cpp`)
 * are the fallback used when no visitor exists for a language and walked the
 * CST recursively without any bound — a pathologically deep AST would overflow
 * the indexer's 512 KB worker stack, which the FFI try/catch cannot recover.
 *
 * Asserted here:
 *   1. a normal file translates and reports nothing (the bound must not fire
 *      on ordinary code);
 *   2. a pathologically nested file still translates — no crash — and the
 *      truncation is reported exactly once;
 *   3. the bound is wired per translator, not only for one language.
 */

#include "../src/ir/ir_translator.h"
#include "../src/ir/semantic_unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

#include <tree_sitter/api.h>

static int tests_run = 0;

#define CHECK(cond, msg)                                                    \
	do {                                                                \
		tests_run++;                                                \
		if (!(cond)) {                                              \
			fprintf(stderr, "FAIL [%d]: %s\n", tests_run, msg); \
			exit(1);                                            \
		}                                                           \
	} while (0)

using LangFn = const TSLanguage *(*)();

/// Load `<GRAMMARS_DIR>/tree-sitter-<grammar>.so`. The handle is left open for
/// the process lifetime.
static const TSLanguage *loadLanguage(const char *grammar, const char *symbol)
{
	const char *dirs[] = { getenv("GRAMMARS_DIR"), "../grammars",
			       "grammars", nullptr };
	for (int i = 0; dirs[i] != nullptr; i++) {
		std::string path = std::string(dirs[i]) + "/tree-sitter-" +
				   grammar + ".so";
		void *handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
		if (!handle)
			continue;
		auto fn = reinterpret_cast<LangFn>(dlsym(handle, symbol));
		if (fn)
			return fn();
		dlclose(handle);
	}
	return nullptr;
}

/// Deepest node level in `node`'s subtree, so the fixture can assert it really
/// nests deeper than the bound the test is about (a fixture that collapsed
/// would make the test pass for the wrong reason).
static int treeDepth(TSNode node)
{
	const uint32_t n = ts_node_child_count(node);
	int deepest = 0;
	for (uint32_t i = 0; i < n; ++i) {
		const int d = treeDepth(ts_node_child(node, i));
		if (d > deepest)
			deepest = d;
	}
	return deepest + 1;
}

/// Parse `code` and return its CST root depth.
static int parseDepth(const char *grammar, const char *symbol,
		      const std::string &code)
{
	const TSLanguage *lang = loadLanguage(grammar, symbol);
	CHECK(lang != nullptr, "grammar loaded for the depth probe");
	TSParser *parser = ts_parser_new();
	ts_parser_set_language(parser, lang);
	TSTree *tree =
		ts_parser_parse_string(parser, nullptr, code.c_str(),
				       static_cast<uint32_t>(code.size()));
	ts_parser_delete(parser);
	CHECK(tree != nullptr, "probe parse succeeded");
	const int depth = treeDepth(ts_tree_root_node(tree));
	ts_tree_delete(tree);
	return depth;
}

/// Read a whole file, or "" when it cannot be opened.
static std::string readFile(const char *path)
{
	std::string out;
	FILE *f = fopen(path, "r");
	if (!f)
		return out;
	char buf[4096];
	size_t n = 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return out;
}

/// Run the legacy translator for `grammar` over `code`, with stderr captured.
/// \param produced  Set to whether translate() returned a unit.
/// \return Everything the translator wrote to stderr.
static std::string translateCapturing(const char *grammar, const char *symbol,
				      const std::string &code,
				      const char *file_path, bool *produced)
{
	const TSLanguage *lang = loadLanguage(grammar, symbol);
	CHECK(lang != nullptr, "grammar loaded");

	TSParser *parser = ts_parser_new();
	ts_parser_set_language(parser, lang);
	TSTree *tree =
		ts_parser_parse_string(parser, nullptr, code.c_str(),
				       static_cast<uint32_t>(code.size()));
	ts_parser_delete(parser);
	CHECK(tree != nullptr, "parse succeeded");

	const char *log = "/tmp/test_translator_depth_guard.stderr";
	fflush(stderr);
	const int saved = dup(fileno(stderr));
	CHECK(saved >= 0, "stderr duplicated");
	CHECK(freopen(log, "w", stderr) != nullptr, "stderr redirected");

	auto translator = ir::createTranslator(grammar);
	CHECK(translator != nullptr, "translator created");
	ir::TranslationUnit *unit =
		translator->translate(tree, code.c_str(), file_path);
	*produced = unit != nullptr;
	delete unit;

	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	ts_tree_delete(tree);

	return readFile(log);
}

/// `opening` × n + `body` + `closing` × n, so the CST nests n levels deep.
static std::string nest(const char *opening, const char *body,
			const char *closing, int n)
{
	std::string code;
	for (int i = 0; i < n; ++i)
		code += opening;
	code += body;
	for (int i = 0; i < n; ++i)
		code += closing;
	return code;
}

static int countOccurrences(const std::string &hay, const std::string &needle)
{
	int n = 0;
	size_t pos = hay.find(needle);
	while (pos != std::string::npos) {
		++n;
		pos = hay.find(needle, pos + needle.size());
	}
	return n;
}

int main()
{
	const std::string truncated = "kMaxTranslateDepth";

	// ── 1. Ordinary code is not truncated ───────────────────────────────
	{
		bool produced = false;
		const std::string err = translateCapturing(
			"c", "tree_sitter_c", "int f(void) { return 1; }\n",
			"/t/shallow.c", &produced);
		CHECK(produced, "a normal file must still translate");
		CHECK(countOccurrences(err, truncated) == 0,
		      "the recursion bound must not fire on ordinary code");
		printf("Test 1 (normal file untruncated): PASS\n");
	}

	// ── 2. Pathological nesting is bounded and reported once ────────────
	{
		// Nested BLOCKS, not nested parentheses: the translator only
		// descends into the container types it knows (compound_statement,
		// statements, declarations), so a paren chain never reaches the
		// recursion this bound protects — and could not overflow it.
		const std::string code = std::string("int f(void) { ") +
					 nest("{", "int x = 0;", "}", 900) +
					 " }\n";
		bool produced = false;
		const std::string err = translateCapturing(
			"c", "tree_sitter_c", code, "/t/deep.c", &produced);
		CHECK(parseDepth("c", "tree_sitter_c", code) >
			      ir::kMaxTranslateDepth,
		      "fixture check: the file must nest deeper than the "
		      "bound, or this test would pass vacuously");
		CHECK(produced,
		      "a pathologically nested file must still translate (no "
		      "stack overflow)");
		CHECK(countOccurrences(err, truncated) == 1,
		      "the truncation must be reported exactly once per file");
		printf("Test 2 (deep C file bounded + reported once): PASS\n");
	}

	// ── 3. The bound is wired per translator ────────────────────────────
	{
		// Python has no braces: nest `if` statements by indentation.
		std::string code;
		for (int i = 0; i < 900; ++i)
			code += std::string(i, '\t') + "if True:\n";
		code += std::string(900, '\t') + "pass\n";
		bool produced = false;
		const std::string err =
			translateCapturing("python", "tree_sitter_python", code,
					   "/t/deep.py", &produced);
		CHECK(produced, "the python translator must also complete");
		CHECK(countOccurrences(err, truncated) == 1,
		      "the python translator must apply the same bound (the guard "
		      "belongs to every translator, not just C)");
		printf("Test 3 (bound wired per translator): PASS\n");
	}

	printf("\n=== test_translator_depth_guard PASSED (%d assertions) ===\n",
	       tests_run);
	return 0;
}
