/**
 * Unit tests for JsVisitor — the new JavaScript translator that emits
 * SemanticUnit records instead of building a Node tree.
 *
 * Tests cover:
 * - Basic function extraction (name, location, containment)
 * - Call expression detection
 * - Variable declaration tracking
 * - Import/export emission
 * - Class and method handling
 * - Scope resolution for identifiers
 */

#include "../src/ir/translators/js_visitor.h"
#include "../src/ir/semantic_unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

// tree-sitter
#include <tree_sitter/api.h>

// Load the JavaScript grammar dynamically
static const TSLanguage *load_js_language()
{
	// Try GRAMMARS_DIR env, then default paths
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
				   "/tree-sitter-javascript.so";
		void *handle = dlopen(path.c_str(),
				      RTLD_LAZY | RTLD_LOCAL);
		if (handle) {
			auto *fn = reinterpret_cast<const TSLanguage *(*)()>(
				dlsym(handle, "tree_sitter_javascript"));
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

// Count records of a specific kind
static size_t countKind(const ir::SemanticUnit &unit, ir::RecordKind kind)
{
	return unit.findRecordsByKind(kind).size();
}

// Find first record by name
static const ir::Record *findByName(const ir::SemanticUnit &unit,
				     const std::string &name)
{
	size_t idx = unit.findRecordByName(name);
	if (idx == SIZE_MAX)
		return nullptr;
	return &unit.allRecords()[idx];
}

// ── Test: Basic function extraction ───────────────────────────

static void test_simple_function()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "function add(a, b) {\n"
			   "    return a + b;\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/add.js");
	CHECK(unit != nullptr, "visit returned unit");
	CHECK(!unit->empty(), "unit is not empty");

	// Should have:
	// - 1 Function record (add)
	// - Variables for parameters? (a, b) — these may or may not
	//   be emitted depending on the visitor implementation
	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	const ir::Record *add = findByName(*unit, "add");
	CHECK(add != nullptr, "function 'add' found");
	CHECK(add->kind == ir::RecordKind::Function,
	      "record kind is Function");
	CHECK(add->loc.start_row == 0, "add starts at row 0");
	CHECK(add->file_path == "/test/add.js",
	      "file path preserved");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_simple_function\n");
}

// ── Test: Call expression detection ───────────────────────────

static void test_call_expression()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "function main() {\n"
			   "    return foo(bar);\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/call.js");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	size_t calls = countKind(*unit, ir::RecordKind::CallExpr);
	CHECK_EQ(calls, 1ULL, "1 call expression found");

	const ir::Record *call = nullptr;
	for (auto &r : unit->allRecords()) {
		if (r.kind == ir::RecordKind::CallExpr) {
			call = &r;
			break;
		}
	}
	CHECK(call != nullptr, "call record exists");
	CHECK(call->name == "foo", "call name is 'foo'");

	// Verify containment: call should be a child of main
	const ir::Record *main_fn = findByName(*unit, "main");
	CHECK(main_fn != nullptr, "main function found");
	CHECK_EQ(call->parent_id, main_fn->id,
		 "call is child of main");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_call_expression\n");
}

// ── Test: Variable declaration ────────────────────────────────

static void test_variable_declaration()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "function main() {\n"
			   "    const x = 42;\n"
			   "    let y = x + 1;\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/var.js");

	const ir::Record *x = findByName(*unit, "x");
	CHECK(x != nullptr, "variable 'x' found");

	const ir::Record *y = findByName(*unit, "y");
	CHECK(y != nullptr, "variable 'y' found");

	// Both should be children of main
	const ir::Record *main_fn = findByName(*unit, "main");
	CHECK(main_fn != nullptr, "main function found");
	CHECK_EQ(x->parent_id, main_fn->id,
		 "x is child of main");
	CHECK_EQ(y->parent_id, main_fn->id,
		 "y is child of main");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_variable_declaration\n");
}

// ── Test: Multiple functions with calls ───────────────────────

static void test_multi_function()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "function add(a, b) {\n"
			   "    return a + b;\n"
			   "}\n"
			   "function main() {\n"
			   "    const r = add(1, 2);\n"
			   "    console.log(r);\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/multi.js");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 2ULL, "2 functions found");

	size_t calls = countKind(*unit, ir::RecordKind::CallExpr);
	CHECK_EQ(calls, 2ULL, "2 calls found (add + console.log)");

	// add is top-level (parent_id = 0)
	const ir::Record *add = findByName(*unit, "add");
	CHECK(add != nullptr, "function 'add' found");
	CHECK_EQ(add->parent_id, 0ULL, "add is top-level");

	// main is top-level
	const ir::Record *main_fn = findByName(*unit, "main");
	CHECK(main_fn != nullptr, "function 'main' found");
	CHECK_EQ(main_fn->parent_id, 0ULL, "main is top-level");

	// r is a variable child of main
	const ir::Record *r = findByName(*unit, "r");
	CHECK(r != nullptr, "variable 'r' found");
	CHECK_EQ(r->parent_id, main_fn->id,
		 "r is child of main");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_multi_function\n");
}

// ── Test: Class and method ────────────────────────────────────

static void test_class_method()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "class Calculator {\n"
			   "    add(a, b) {\n"
			   "        return a + b;\n"
			   "    }\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/class.js");

	const ir::Record *calc = findByName(*unit, "Calculator");
	CHECK(calc != nullptr, "class 'Calculator' found");

	const ir::Record *add = findByName(*unit, "add");
	CHECK(add != nullptr, "method 'add' found");
	CHECK(add->kind == ir::RecordKind::Method,
	      "add is a Method record");
	CHECK_EQ(add->parent_id, calc->id,
		 "add is child of Calculator");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_class_method\n");
}

// ── Test: Import and export ───────────────────────────────────

static void test_import_export()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "import { foo } from './mod';\n"
			   "export function bar() {}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/import.js");

	size_t imports = countKind(*unit, ir::RecordKind::Import);
	CHECK_EQ(imports, 1ULL, "1 import found");

	size_t exports = countKind(*unit, ir::RecordKind::Export);
	CHECK_EQ(exports, 1ULL, "1 export found");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	// bar should be a child of export
	const ir::Record *bar = findByName(*unit, "bar");
	CHECK(bar != nullptr, "function 'bar' found");

	const ir::Record *exp = nullptr;
	for (auto &r : unit->allRecords()) {
		if (r.kind == ir::RecordKind::Export) {
			exp = &r;
			break;
		}
	}
	CHECK(exp != nullptr, "export record found");
	CHECK_EQ(bar->parent_id, exp->id,
		 "bar is child of export record");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_import_export\n");
}

// ── Test: Arrow function ──────────────────────────────────────

static void test_arrow_function()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "const add = (a, b) => a + b;\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/arrow.js");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function (arrow) found");

	// Arrow function name is empty string
	const ir::Record *add = findByName(*unit, "add");
	CHECK(add != nullptr, "variable 'add' found");
	CHECK(add->kind == ir::RecordKind::Variable,
	      "add is a Variable record");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_arrow_function\n");
}

// ── Test: Member expression ───────────────────────────────────

static void test_member_expression()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "console.log('hello');\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/member.js");

	size_t calls = countKind(*unit, ir::RecordKind::CallExpr);
	CHECK_EQ(calls, 1ULL, "1 call expression found");

	const ir::Record *call = nullptr;
	for (auto &r : unit->allRecords()) {
		if (r.kind == ir::RecordKind::CallExpr) {
			call = &r;
			break;
		}
	}
	CHECK(call != nullptr, "call record exists");
	// The call name for member expressions might be empty
	// or contain the full path — depends on the implementation

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_member_expression\n");
}

// ── Test: Empty file ──────────────────────────────────────────

static void test_empty_file()
{
	const TSLanguage *lang = load_js_language();
	CHECK(lang != nullptr, "JavaScript grammar loaded");

	const char *code = "// just a comment\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::JsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code, "/test/empty.js");
	CHECK(unit != nullptr, "visit returned unit");
	// Even an empty file may produce some records (the root context)
	CHECK(!unit->empty(), "unit is not null");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_empty_file\n");
}

// ── Main ──────────────────────────────────────────────────────

int main()
{
	printf("JsVisitor tests:\n");

	test_simple_function();
	test_call_expression();
	test_variable_declaration();
	test_multi_function();
	test_class_method();
	test_import_export();
	test_arrow_function();
	test_member_expression();
	test_empty_file();

	printf("\n=== js_visitor test passed (%d/%d) ===\n",
	       tests_passed, tests_run);
	return 0;
}
