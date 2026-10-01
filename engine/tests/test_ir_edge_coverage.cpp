/**
 * Unit tests for the IR coverage gaps fixed for
 * 2026-09-27 review D1-2 (README "Verified" column).
 *
 * Why this file exists: the per-language E2E harness (test_e2e.h) only asserts
 * that the JSON contains the KEYS "callers"/"callees"/"total_nodes" — never
 * that an edge exists. A translator that emitted zero call edges therefore
 * passed the whole suite, which is how these gaps survived a green build.
 * These cases assert the emitted RECORDS directly, at the visitor level:
 *
 *   1. Rust    — macro_invocation emits a CallExpr; builtin macros do not.
 *   2. Go      — `a, b := ...` emits/defines the variables (LHS lives inside
 *                an expression_list, which the old scan never reached).
 *   3. C++     — in-class method declarations, operator== and ~Point are
 *                recorded, and out-of-class definitions keep their scope.
 *   4. Python  — a chained callee `self.helper.compute()` resolves to the
 *                LAST segment ("compute"), not the receiver ("helper").
 *   5. Java    — `implements` emits InterfaceImpl (names live in a type_list).
 *   6. TS/TSX  — `implements` emits InterfaceImpl; a self-closing JSX element's
 *                attribute expressions are visited.
 *   7. C++/TS  — a user function whose name collides with a builtin is not
 *                dropped by the builtin-name filter.
 */

#include "../src/ir/semantic_unit.h"
#include "../src/ir/translators/cpp_visitor.h"
#include "../src/ir/translators/go_visitor.h"
#include "../src/ir/translators/java_visitor.h"
#include "../src/ir/translators/python_visitor.h"
#include "../src/ir/translators/rust_visitor.h"
#include "../src/ir/translators/ts_visitor.h"
#include "../src/ir/translators/tsx_visitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>

#include <tree_sitter/api.h>

// ── Harness ───────────────────────────────────────────────────

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg)                                                    \
	do {                                                                \
		tests_run++;                                                \
		if (!(cond)) {                                              \
			fprintf(stderr, "FAIL [%d]: %s\n", tests_run, msg); \
			exit(1);                                            \
		}                                                           \
		tests_passed++;                                             \
	} while (0)

using LangFn = const TSLanguage *(*)();

/// Load `<GRAMMARS_DIR>/tree-sitter-<grammar>.so` and return its language.
/// The handle is intentionally left open for the process lifetime.
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

/// Parse `code` with the given grammar and run `visitor` over it.
/// Ownership of the returned unit passes to the caller.
static ir::SemanticUnit *runVisitor(ir::JsVisitor &visitor, const char *grammar,
				    const char *symbol, const char *code,
				    const char *file_path)
{
	const TSLanguage *lang = loadLanguage(grammar, symbol);
	CHECK(lang != nullptr, "grammar loaded");

	TSParser *parser = ts_parser_new();
	ts_parser_set_language(parser, lang);
	TSTree *tree = ts_parser_parse_string(
		parser, nullptr, code, static_cast<uint32_t>(strlen(code)));
	ts_parser_delete(parser);
	CHECK(tree != nullptr, "parse succeeded");

	ir::SemanticUnit *unit = visitor.visit(tree, code, file_path);
	ts_tree_delete(tree);
	return unit;
}

/// True when a record of `kind` is named `name`.
static bool hasNamed(const ir::SemanticUnit &unit, ir::RecordKind kind,
		     const std::string &name)
{
	for (size_t idx : unit.findRecordsByKind(kind)) {
		if (unit.allRecords()[idx].name == name)
			return true;
	}
	return false;
}

/// Number of records of `kind` named `name`.
static int countNamed(const ir::SemanticUnit &unit, ir::RecordKind kind,
		      const std::string &name)
{
	int n = 0;
	for (size_t idx : unit.findRecordsByKind(kind)) {
		if (unit.allRecords()[idx].name == name)
			n++;
	}
	return n;
}

/// True when any record carries `qualified_name`.
static bool hasQualifiedName(const ir::SemanticUnit &unit,
			     const std::string &qname)
{
	for (const ir::Record &r : unit.allRecords()) {
		if (r.qualified_name == qname)
			return true;
	}
	return false;
}

/// True when `impl_type` implements `iface` (an InterfaceImpl record).
static bool hasInterfaceImpl(const ir::SemanticUnit &unit,
			     const std::string &impl_type,
			     const std::string &iface)
{
	for (size_t idx :
	     unit.findRecordsByKind(ir::RecordKind::InterfaceImpl)) {
		const ir::Record &r = unit.allRecords()[idx];
		if (r.name == impl_type && r.type_name == iface)
			return true;
	}
	return false;
}

// ── 1. Rust macro invocations ─────────────────────────────────

static void test_rust_macro_invocation_emits_call()
{
	const char *code = "fn do_work() {}\n"
			   "macro_rules! do_work { () => {}; }\n"
			   "fn caller() { do_work!(); }\n";

	ir::RustVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "rust", "tree_sitter_rust",
					    code, "/test/macro.rs");

	CHECK(hasNamed(*unit, ir::RecordKind::CallExpr, "do_work"),
	      "macro_invocation must emit a CallExpr (macro calls are calls)");

	delete unit;
	printf("  ✓ test_rust_macro_invocation_emits_call\n");
}

static void test_rust_builtin_macro_filtered()
{
	const char *code = "fn caller() { println!(\"hello\"); }\n";

	ir::RustVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "rust", "tree_sitter_rust",
					    code, "/test/builtin_macro.rs");

	CHECK(!hasNamed(*unit, ir::RecordKind::CallExpr, "println"),
	      "builtin macro println! must not create a call record");

	delete unit;
	printf("  ✓ test_rust_builtin_macro_filtered\n");
}

// ── 2. Go short variable declarations ─────────────────────────

static void test_go_short_var_defines_variables()
{
	// The LHS of `:=` is wrapped in an expression_list, so the previous
	// direct-children scan emitted no Variable at all.
	const char *code = "package main\n"
			   "func compute() int { return 1 }\n"
			   "func caller() {\n"
			   "    a, b := compute(), compute()\n"
			   "    _, c := compute(), compute()\n"
			   "}\n";

	ir::GoVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "go", "tree_sitter_go",
					    code, "/test/shortvar.go");

	CHECK(hasNamed(*unit, ir::RecordKind::Variable, "a"),
	      "`a, b := ...` must emit Variable a");
	CHECK(hasNamed(*unit, ir::RecordKind::Variable, "b"),
	      "`a, b := ...` must emit Variable b");
	CHECK(hasNamed(*unit, ir::RecordKind::Variable, "c"),
	      "`_, c := ...` must emit Variable c");
	CHECK(!hasNamed(*unit, ir::RecordKind::Variable, "_"),
	      "the blank identifier binds no symbol");

	delete unit;
	printf("  ✓ test_go_short_var_defines_variables\n");
}

static void test_go_blank_identifier_binds_nothing()
{
	// `_` is a discard, not a symbol, in every form — and discarding the name
	// must not discard the initializer: its calls still belong to the
	// enclosing function. Both halves regressed differently before:
	// `_, x := f()` skipped the name but a plain `_, x = f()` emitted a `_`
	// entity, and `var x = f()` dropped the call entirely.
	const char *code = "package main\n"
			   "func compute() int { return 1 }\n"
			   "func caller() int {\n"
			   "    _, c := compute(), compute()\n"
			   "    var _ = compute()\n"
			   "    var x = compute()\n"
			   "    _, c = compute(), compute()\n"
			   "    return c + x\n"
			   "}\n";

	ir::GoVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "go", "tree_sitter_go",
					    code, "/test/blank.go");

	CHECK(!hasNamed(*unit, ir::RecordKind::Variable, "_"),
	      "the blank identifier must not become a symbol");
	CHECK(countNamed(*unit, ir::RecordKind::CallExpr, "compute") == 6,
	      "every compute() call must be recorded — including the ones in a "
	      "blank-identifier or var-declaration initializer");

	delete unit;
	printf("  ✓ test_go_blank_identifier_binds_nothing\n");
}

// ── 3. C++ in-class declarations, operators, destructors ──────

static void test_cpp_class_member_declarations()
{
	// Point's members are DECLARED only (no definition anywhere in the file).
	// Line's members are declared in-class AND defined out-of-class.
	const char *code =
		"class Point {\n"
		"public:\n"
		"    void foo();\n"
		"    bool operator==(const Point &o) const;\n"
		"    ~Point();\n"
		"};\n"
		"class Line {\n"
		"public:\n"
		"    bool operator==(const Line &o) const;\n"
		"    ~Line();\n"
		"    int len;\n"
		"};\n"
		"bool Line::operator==(const Line &o) const { return len == o.len; }\n"
		"Line::~Line() {}\n";

	ir::CppVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "cpp", "tree_sitter_cpp",
					    code, "/test/members.cpp");

	CHECK(hasNamed(*unit, ir::RecordKind::Method, "foo"),
	      "in-class declaration `void foo();` must emit a Method");
	CHECK(hasNamed(*unit, ir::RecordKind::Method, "operator=="),
	      "in-class `operator==` declaration must be named");
	CHECK(hasNamed(*unit, ir::RecordKind::Method, "~Point"),
	      "in-class destructor declaration must be named");
	CHECK(hasQualifiedName(*unit, "Line::operator=="),
	      "out-of-class definition must carry its scope (type_identifier, "
	      "not identifier)");
	CHECK(hasQualifiedName(*unit, "Line::~Line"),
	      "out-of-class destructor definition must carry its scope");
	// A declaration whose definition is in the SAME file must not be
	// emitted a second time: two same-name/same-arity candidates make the
	// Resolver's ambiguity gate abstain and the CALLS edge is lost
	// (test_qualified_id_ast covers that edge).
	CHECK(!hasNamed(*unit, ir::RecordKind::Method, "~Line"),
	      "declaration + same-file definition must not create a duplicate");

	delete unit;
	printf("  ✓ test_cpp_class_member_declarations\n");
}

static void test_cpp_local_function_decl_is_not_a_member()
{
	// A LOCAL function declaration inside a method body is not a class member.
	// Dispatching `declaration` nodes on the enclosing class name (an earlier
	// revision) also matched these and emitted a phantom `Point::helper`.
	const char *code = "class Point {\n"
			   "public:\n"
			   "    void run();\n"
			   "    ~Point();\n"
			   "};\n"
			   "void Point::run() {\n"
			   "    int helper(int);\n"
			   "    (void)helper(1);\n"
			   "}\n"
			   "Point::~Point() {}\n";

	ir::CppVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "cpp", "tree_sitter_cpp",
					    code, "/test/local.cpp");

	CHECK(hasQualifiedName(*unit, "Point::run"),
	      "the out-of-class definition must carry its scope");
	CHECK(!hasNamed(*unit, ir::RecordKind::Method, "run"),
	      "a declaration whose definition is in the same file stays "
	      "deduplicated");
	CHECK(!hasNamed(*unit, ir::RecordKind::Method, "helper") &&
		      !hasQualifiedName(*unit, "Point::helper"),
	      "a local function declaration must not become a class member");
	CHECK(!hasNamed(*unit, ir::RecordKind::Method, "~Point"),
	      "a declaration whose definition is in the same file stays "
	      "deduplicated");
	CHECK(hasQualifiedName(*unit, "Point::~Point"),
	      "the destructor definition must still be recorded");

	delete unit;
	printf("  ✓ test_cpp_local_function_decl_is_not_a_member\n");
}

// ── 4. Python chained attribute calls ─────────────────────────

static void test_python_chained_call_name()
{
	// `self.helper.compute()` must resolve to "compute" — the old code
	// returned the INNER receiver segment ("helper").
	const char *code = "class Worker:\n"
			   "    def compute(self, x):\n"
			   "        return x * 2\n"
			   "    def run(self):\n"
			   "        return self.helper.compute(10)\n";

	ir::PythonVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "python",
					    "tree_sitter_python", code,
					    "/test/chained.py");

	CHECK(hasNamed(*unit, ir::RecordKind::CallExpr, "compute"),
	      "chained call must resolve to the LAST segment (compute)");
	CHECK(!hasNamed(*unit, ir::RecordKind::CallExpr, "helper"),
	      "chained call must NOT resolve to the receiver (helper)");

	delete unit;
	printf("  ✓ test_python_chained_call_name\n");
}

// ── 5. Java implements ────────────────────────────────────────

static void test_java_implements_emits_interface_impl()
{
	const char *code =
		"interface Drawable { void draw(); }\n"
		"class Circle implements Drawable, Comparable<Circle> {\n"
		"    public void draw() {}\n"
		"    public int compareTo(Circle o) { return 0; }\n"
		"}\n";

	ir::JavaVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "java", "tree_sitter_java",
					    code, "/test/Circle.java");

	CHECK(hasInterfaceImpl(*unit, "Circle", "Drawable"),
	      "`implements Drawable` must emit InterfaceImpl (names live in "
	      "type_list)");
	CHECK(hasInterfaceImpl(*unit, "Circle", "Comparable"),
	      "generic implements clause records the base type");

	delete unit;
	printf("  ✓ test_java_implements_emits_interface_impl\n");
}

// ── 6. TypeScript / TSX ───────────────────────────────────────

static void test_ts_implements_emits_interface_impl()
{
	const char *code =
		"interface Shape { area(): number; }\n"
		"class Square implements Shape, Comparable<Square> {\n"
		"    area() { return 1; }\n"
		"    compareTo(o: Square) { return 0; }\n"
		"}\n";

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "typescript",
					    "tree_sitter_typescript", code,
					    "/test/square.ts");

	CHECK(hasInterfaceImpl(*unit, "Square", "Shape"),
	      "TS `implements Shape` must emit InterfaceImpl");
	CHECK(hasInterfaceImpl(*unit, "Square", "Comparable"),
	      "generic implements clause records the base type");

	delete unit;
	printf("  ✓ test_ts_implements_emits_interface_impl\n");
}

static void test_tsx_self_closing_attribute_call()
{
	const char *code = "function bar() { return 1; }\n"
			   "const el = <Foo onClick={bar()} />;\n";

	ir::TsxVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "tsx", "tree_sitter_tsx",
					    code, "/test/elem.tsx");

	CHECK(hasNamed(*unit, ir::RecordKind::CallExpr, "bar"),
	      "call inside a self-closing JSX attribute must be visited");
	// The tag name is not code: recursing wholesale would emit it.
	CHECK(!hasNamed(*unit, ir::RecordKind::Variable, "Foo"),
	      "the JSX tag name must not become a Variable");

	delete unit;
	printf("  ✓ test_tsx_self_closing_attribute_call\n");
}

// ── 7. Builtin-name exemption (collectDefinedNames) ───────────

static void test_cpp_user_function_shadowing_builtin()
{
	// `free` is in isCBuiltin(); a function this file defines must survive
	// that filter, which requires collectDefinedNames to have run.
	const char *code = "void free(void *p) { (void)p; }\n"
			   "void caller(void *p) { free(p); }\n";

	ir::CppVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "cpp", "tree_sitter_cpp",
					    code, "/test/free.cpp");

	CHECK(hasNamed(*unit, ir::RecordKind::CallExpr, "free"),
	      "user-defined `free()` call must not be filtered as a builtin");

	delete unit;
	printf("  ✓ test_cpp_user_function_shadowing_builtin\n");
}

static void test_ts_user_function_shadowing_builtin()
{
	// `String` is in isJsBuiltin(); the file defines its own.
	const char *code = "function String(x: number) { return x; }\n"
			   "function caller() { return String(1); }\n";

	ir::TsVisitor visitor;
	ir::SemanticUnit *unit = runVisitor(visitor, "typescript",
					    "tree_sitter_typescript", code,
					    "/test/shadow.ts");

	CHECK(hasNamed(*unit, ir::RecordKind::CallExpr, "String"),
	      "user-defined `String()` call must not be filtered as a builtin");

	delete unit;
	printf("  ✓ test_ts_user_function_shadowing_builtin\n");
}

// ── Main ──────────────────────────────────────────────────────

int main()
{
	printf("IR edge coverage tests (2026-09-27 review D1-2):\n");

	test_rust_macro_invocation_emits_call();
	test_rust_builtin_macro_filtered();
	test_go_short_var_defines_variables();
	test_go_blank_identifier_binds_nothing();
	test_cpp_class_member_declarations();
	test_cpp_local_function_decl_is_not_a_member();
	test_python_chained_call_name();
	test_java_implements_emits_interface_impl();
	test_ts_implements_emits_interface_impl();
	test_tsx_self_closing_attribute_call();
	test_cpp_user_function_shadowing_builtin();
	test_ts_user_function_shadowing_builtin();

	printf("\n=== ir_edge_coverage test passed (%d/%d) ===\n", tests_passed,
	       tests_run);
	return 0;
}
