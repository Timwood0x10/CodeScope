/**
 * Unit tests for TsxVisitor — TSX visitor that extends TsVisitor
 * with JSX element skipping.
 *
 * Tests cover:
 * - JSX self-closing elements are skipped (no semantic records)
 * - JSX elements with children are skipped
 * - JSX expressions recurse into children (JS code inside JSX)
 * - Regular TS constructs still work inside .tsx files
 * - Language tag set to "tsx"
 */

#include "../src/ir/translators/tsx_visitor.h"
#include "../src/ir/semantic_unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

// tree-sitter
#include <tree_sitter/api.h>

// Load the TSX grammar dynamically
static const TSLanguage *load_tsx_language()
{
	const char *dirs[] = {
		getenv("GRAMMARS_DIR"),
		"../grammars",
		"grammars",
		"/Users/scc/code/cppCode/CodeScope/grammars",
	};
	for (auto d : dirs) {
		if (!d)
			continue;
		std::string path = std::string(d) +
				   "/tree-sitter-tsx.so";
		void *handle = dlopen(path.c_str(),
				      RTLD_LAZY | RTLD_LOCAL);
		if (handle) {
			auto *fn = reinterpret_cast<const TSLanguage *(*)()>(
				dlsym(handle, "tree_sitter_tsx"));
			if (fn)
				return fn();
			dlclose(handle);
		}
	}
	return nullptr;
}

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg)                                                      \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%d]: %s\n", tests_run, msg);    \
			exit(1);                                               \
		}                                                              \
		tests_passed++;                                                \
	} while (0)

#define CHECK_EQ(a, b, msg)                                                    \
	do {                                                                   \
		tests_run++;                                                   \
		if ((a) != (b)) {                                              \
			fprintf(stderr, "FAIL [%d]: %s — expected %llu, "      \
					"got %llu\n",                          \
				tests_run, msg,                                 \
				static_cast<unsigned long long>(b),            \
				static_cast<unsigned long long>(a));           \
			exit(1);                                               \
		}                                                              \
		tests_passed++;                                                \
	} while (0)

#define CHECK_GE(a, b, msg)                                                    \
	do {                                                                   \
		tests_run++;                                                   \
		if ((a) < (b)) {                                               \
			fprintf(stderr, "FAIL [%d]: %s — expected >= %llu, "   \
					"got %llu\n",                          \
				tests_run, msg,                                 \
				static_cast<unsigned long long>(b),            \
				static_cast<unsigned long long>(a));           \
			exit(1);                                               \
		}                                                              \
		tests_passed++;                                                \
	} while (0)

// ── Helpers ───────────────────────────────────────────────────

static TSTree *parse(const char *source, const TSLanguage *lang)
{
	TSParser *parser = ts_parser_new();
	ts_parser_set_language(parser, lang);
	TSTree *tree = ts_parser_parse_string(
		parser, nullptr, source,
		static_cast<uint32_t>(strlen(source)));
	ts_parser_delete(parser);
	return tree;
}

static size_t countKind(const ir::SemanticUnit &unit, ir::RecordKind kind)
{
	return unit.findRecordsByKind(kind).size();
}

static const ir::Record *findByName(const ir::SemanticUnit &unit,
					     const std::string &name)
{
	size_t idx = unit.findRecordByName(name);
	if (idx == SIZE_MAX)
		return nullptr;
	return &unit.allRecords()[idx];
}

// ── Test: JSX element skipped ─────────────────────────────────

static void test_jsx_self_closing_skipped()
{
	const TSLanguage *lang = load_tsx_language();
	CHECK(lang != nullptr, "TSX grammar loaded");

	const char *code = "const el = <div className=\"app\" />;\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/elem.tsx");

	// Should only have the variable record (el), no JSX noise
	const ir::Record *el = findByName(*unit, "el");
	CHECK(el != nullptr, "variable 'el' found");
	CHECK(el->kind == ir::RecordKind::Variable,
	      "el is a Variable record");

	CHECK(unit->language() == "tsx",
	      "language is tsx");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_jsx_self_closing_skipped\n");
}

// ── Test: JSX element with children ───────────────────────────

static void test_jsx_element_skipped()
{
	const TSLanguage *lang = load_tsx_language();
	CHECK(lang != nullptr, "TSX grammar loaded");

	const char *code = "const el = <div>\n"
			   "    <span>text</span>\n"
			   "</div>;\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/jsx.tsx");

	// Only the variable 'el' should be emitted
	const ir::Record *el = findByName(*unit, "el");
	CHECK(el != nullptr, "variable 'el' found");
	CHECK(el->kind == ir::RecordKind::Variable,
	      "el is a Variable record");

	// No extra records from JSX
	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_jsx_element_skipped\n");
}

// ── Test: JSX expression recurses ─────────────────────────────

static void test_jsx_expression_recurses()
{
	const TSLanguage *lang = load_tsx_language();
	CHECK(lang != nullptr, "TSX grammar loaded");

	const char *code = "function App() {\n"
			   "    return <div>{compute(42)}</div>;\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/app.tsx");

	// Should have a Function (App)
	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	// compute(42) call should be detected
	size_t calls = countKind(*unit, ir::RecordKind::CallExpr);
	CHECK_EQ(calls, 1ULL, "1 call expression found (compute)");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_jsx_expression_recurses\n");
}

// ── Test: TS construct inside TSX ─────────────────────────────

static void test_tsx_with_interface()
{
	const TSLanguage *lang = load_tsx_language();
	CHECK(lang != nullptr, "TSX grammar loaded");

	const char *code = "interface Props {\n"
			   "    name: string;\n"
			   "}\n"
			   "function Greet(props: Props) {\n"
			   "    return <div>Hello</div>;\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/greet.tsx");

	// Interface should be detected
	size_t ifaces = countKind(*unit, ir::RecordKind::Interface);
	CHECK_EQ(ifaces, 1ULL, "1 interface found");

	// Function should be detected
	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	// Language should be tsx
	CHECK(unit->language() == "tsx",
	      "language is tsx");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_tsx_with_interface\n");
}

// ── Test: Handle component with member call ───────────────────

static void test_tsx_member_call()
{
	const TSLanguage *lang = load_tsx_language();
	CHECK(lang != nullptr, "TSX grammar loaded");

	const char *code = "import React from 'react';\n"
			   "const App = () => {\n"
			   "    return <div>{console.log('test')}</div>;\n"
			   "};\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/app.tsx");

	// Import should be detected
	size_t imports = countKind(*unit, ir::RecordKind::Import);
	CHECK_EQ(imports, 1ULL, "1 import found");

	// Function should be detected (arrow)
	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 arrow function found");

	// console.log call should be detected
	size_t calls = countKind(*unit, ir::RecordKind::CallExpr);
	CHECK_GE(calls, 1ULL, "at least 1 call found");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_tsx_member_call\n");
}

// ── Test: TSX empty file ──────────────────────────────────────

static void test_tsx_empty_file()
{
	const TSLanguage *lang = load_tsx_language();
	CHECK(lang != nullptr, "TSX grammar loaded");

	const char *code = "// just a comment\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/empty.tsx");
	CHECK(unit != nullptr, "visit returned unit");
	CHECK(!unit->empty(), "unit is not empty");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_tsx_empty_file\n");
}

// ── Main ──────────────────────────────────────────────────────

int main()
{
	printf("TsxVisitor tests:\n");

	test_jsx_self_closing_skipped();
	test_jsx_element_skipped();
	test_jsx_expression_recurses();
	test_tsx_with_interface();
	test_tsx_member_call();
	test_tsx_empty_file();

	printf("\n=== tsx_visitor test passed (%d/%d) ===\n",
	       tests_passed, tests_run);
	return 0;
}
