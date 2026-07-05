/**
 * Unit tests for TsVisitor — TypeScript visitor that extends JsVisitor
 * with interface, type alias, and enum support.
 *
 * Tests cover:
 * - Interface declaration → Interface records
 * - Type alias → TypeAlias records
 * - Enum declaration → Enum records
 * - TS-specific pass-through (type annotations, etc.)
 * - Shared JS construct inheritance (function, class, call, variable)
 * - Scope resolution for TS constructs
 * - Language tag set to "typescript"
 */

#include "../src/ir/translators/ts_visitor.h"
#include "../src/ir/semantic_unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

// tree-sitter
#include <tree_sitter/api.h>

// Load the TypeScript grammar dynamically
static const TSLanguage *load_ts_language()
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
				   "/tree-sitter-typescript.so";
		void *handle = dlopen(path.c_str(),
				      RTLD_LAZY | RTLD_LOCAL);
		if (handle) {
			auto *fn = reinterpret_cast<const TSLanguage *(*)()>(
				dlsym(handle, "tree_sitter_typescript"));
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

// ── Test: Interface declaration ───────────────────────────────

static void test_interface_declaration()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "interface User {\n"
			   "    name: string;\n"
			   "    age: number;\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/interface.ts");

	size_t ifaces = countKind(*unit, ir::RecordKind::Interface);
	CHECK_EQ(ifaces, 1ULL, "1 interface found");

	const ir::Record *user = findByName(*unit, "User");
	CHECK(user != nullptr, "interface 'User' found");
	CHECK(user->kind == ir::RecordKind::Interface,
	      "record kind is Interface");
	CHECK_EQ(user->parent_id, 0ULL, "User is top-level");
	CHECK(user->file_path == "/test/interface.ts",
	      "file path preserved");
	CHECK(unit->language() == "typescript",
	      "language is typescript");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_interface_declaration\n");
}

// ── Test: Type alias declaration ──────────────────────────────

static void test_type_alias_declaration()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "type StringOrNum = string | number;\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/alias.ts");

	size_t aliases = countKind(*unit, ir::RecordKind::TypeAlias);
	CHECK_EQ(aliases, 1ULL, "1 type alias found");

	const ir::Record *alias = findByName(*unit, "StringOrNum");
	CHECK(alias != nullptr, "alias 'StringOrNum' found");
	CHECK(alias->kind == ir::RecordKind::TypeAlias,
	      "record kind is TypeAlias");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_type_alias_declaration\n");
}

// ── Test: Enum declaration ────────────────────────────────────

static void test_enum_declaration()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "enum Color {\n"
			   "    Red,\n"
			   "    Green,\n"
			   "    Blue\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/enum.ts");

	size_t enums = countKind(*unit, ir::RecordKind::Enum);
	CHECK_EQ(enums, 1ULL, "1 enum found");

	const ir::Record *color = findByName(*unit, "Color");
	CHECK(color != nullptr, "enum 'Color' found");
	CHECK(color->kind == ir::RecordKind::Enum,
	      "record kind is Enum");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_enum_declaration\n");
}

// ── Test: TS function (inherited from JsVisitor) ──────────────

static void test_ts_function()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "function greet(name: string): void {\n"
			   "    console.log(name);\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/function.ts");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	const ir::Record *greet = findByName(*unit, "greet");
	CHECK(greet != nullptr, "function 'greet' found");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_ts_function\n");
}

// ── Test: TS class with method (inherited from JsVisitor) ─────

static void test_ts_class_method()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "class Calculator {\n"
			   "    add(a: number, b: number): number {\n"
			   "        return a + b;\n"
			   "    }\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/class.ts");

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
	printf("  ✓ test_ts_class_method\n");
}

// ── Test: TS call expression ──────────────────────────────────

static void test_ts_call_expression()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "function main() {\n"
			   "    const result: number = compute(42);\n"
			   "}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/call.ts");

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
	CHECK(call->name == "compute",
	      "call name is 'compute'");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_ts_call_expression\n");
}

// ── Test: TS import/export ────────────────────────────────────

static void test_ts_import_export()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "import { foo } from './mod';\n"
			   "export function bar(): void {}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/import.ts");

	size_t imports = countKind(*unit, ir::RecordKind::Import);
	CHECK_EQ(imports, 1ULL, "1 import found");

	size_t exports = countKind(*unit, ir::RecordKind::Export);
	CHECK_EQ(exports, 1ULL, "1 export found");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 function found");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_ts_import_export\n");
}

// ── Test: Mixed TS constructs ─────────────────────────────────

static void test_mixed_ts_constructs()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "interface User {\n"
			   "    name: string;\n"
			   "}\n"
			   "type ID = string;\n"
			   "enum Status {\n"
			   "    Active,\n"
			   "    Inactive\n"
			   "}\n"
			   "function process(u: User): void {}\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/mixed.ts");

	CHECK_EQ(countKind(*unit, ir::RecordKind::Interface), 1ULL,
		 "1 interface");
	CHECK_EQ(countKind(*unit, ir::RecordKind::TypeAlias), 1ULL,
		 "1 type alias");
	CHECK_EQ(countKind(*unit, ir::RecordKind::Enum), 1ULL,
		 "1 enum");
	CHECK_EQ(countKind(*unit, ir::RecordKind::Function), 1ULL,
		 "1 function");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_mixed_ts_constructs\n");
}

// ── Test: TS arrow function ───────────────────────────────────

static void test_ts_arrow_function()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "const add = (a: number, b: number): number => a + b;\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/arrow.ts");

	size_t funcs = countKind(*unit, ir::RecordKind::Function);
	CHECK_EQ(funcs, 1ULL, "1 arrow function found");

	const ir::Record *add = findByName(*unit, "add");
	CHECK(add != nullptr, "variable 'add' found");
	CHECK(add->kind == ir::RecordKind::Variable,
	      "add is a Variable record");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_ts_arrow_function\n");
}

// ── Test: TS empty file ───────────────────────────────────────

static void test_ts_empty_file()
{
	const TSLanguage *lang = load_ts_language();
	CHECK(lang != nullptr, "TypeScript grammar loaded");

	const char *code = "// just a comment\n";

	TSTree *tree = parse(code, lang);
	CHECK(tree != nullptr, "parse succeeded");

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = visitor.visit(tree, code,
					       "/test/empty.ts");
	CHECK(unit != nullptr, "visit returned unit");
	CHECK(!unit->empty(), "unit is not empty");

	ts_tree_delete(tree);
	delete unit;
	printf("  ✓ test_ts_empty_file\n");
}

// ── Main ──────────────────────────────────────────────────────

int main()
{
	printf("TsVisitor tests:\n");

	test_interface_declaration();
	test_type_alias_declaration();
	test_enum_declaration();
	test_ts_function();
	test_ts_class_method();
	test_ts_call_expression();
	test_ts_import_export();
	test_mixed_ts_constructs();
	test_ts_arrow_function();
	test_ts_empty_file();

	printf("\n=== ts_visitor test passed (%d/%d) ===\n",
	       tests_passed, tests_run);
	return 0;
}
