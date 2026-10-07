// test_resolver_language_consistency — one call shape, every language, one
// outcome class.
//
// test_graph_call_precision.cpp pins the layered matching on hand-built
// SemanticUnits that merely carry a language LABEL, and test_ir_edge_coverage
// pins what each VISITOR emits as records. Neither covers the property that
// matters here: the visitors disagree about what they record (Go writes a
// receiver, Python a `self` parameter, C++ a declarator, Rust an impl block,
// Java/TS a formal parameter list, and each records its own type spelling), and
// all of them feed ONE resolver. A shape the resolver handles for Go can
// therefore be guessed or dropped for Python without any test noticing.
//
// This test indexes the same shape in six languages and asserts the same
// OUTCOME CLASS for each:
//
//   S1  a call through a parameter whose declared type owns the method, while
//       an unrelated type declares a method of the SAME NAME (the decoy). The
//       call must land on the parameter's type — never on the decoy — or the
//       receiver evidence is being ignored in favour of the name.
//   S2  a call whose receiver type no declaration mentions and whose method
//       name no declaration defines. There is nothing to resolve it to, so
//       there must be NO edge; inventing one is a false positive that every
//       language would inherit from the shared name fallback.
//
// The languages are Go, C++, Java, Python, TypeScript and Rust. JavaScript is
// absent on purpose: it has no type annotations, so S1 cannot be spelled
// without types at all (`test_js_ts_call_facts.cpp` covers its call facts).
#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "test_check.h"
#include "test_engine_handle.h"

namespace
{

/// Write `content` to `path`, failing the test if the file cannot be created.
void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

/// One scalar read; -1 when the query yields no row.
long long scalarSql(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	long long value = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return value;
}

/// The resolution kind of the edge `caller -> callee`, or "(no edge)".
std::string edgeKind(sqlite3 *db, const char *caller, const char *callee)
{
	const char *sql =
		"SELECT r.resolution_kind FROM relation r "
		"JOIN entity se ON se.id = r.source_id "
		"JOIN entity te ON te.id = r.target_id "
		"WHERE r.type = 1 AND se.name = ? AND te.name = ? "
		"ORDER BY r.resolution_kind LIMIT 1";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, caller, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, callee, -1, SQLITE_TRANSIENT);
	std::string out = "(no edge)";
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s)
			out = s;
	}
	sqlite3_finalize(st);
	return out;
}

/// The resolution kind of the edge from `caller` to the method `method` that
/// `owner` declares, or "(no edge)".
///
/// The decoy carries the SAME method name, so a lookup by name alone cannot
/// tell the two methods apart; `entity.qualified_name` is the declaration's
/// owning type plus the method (`CppOwner::Method`, `JavaOwner.method`), so a
/// prefix match identifies the owner in every language without assuming a
/// separator.
std::string edgeKindInType(sqlite3 *db, const char *caller,
			   const char *method, const char *owner)
{
	const char *sql =
		"SELECT r.resolution_kind FROM relation r "
		"JOIN entity se ON se.id = r.source_id "
		"JOIN entity te ON te.id = r.target_id "
		"WHERE r.type = 1 AND se.name = ? AND te.name = ? "
		"AND te.qualified_name LIKE ? || '%' "
		"ORDER BY r.resolution_kind LIMIT 1";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, caller, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, method, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, owner, -1, SQLITE_TRANSIENT);
	std::string out = "(no edge)";
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s)
			out = s;
	}
	sqlite3_finalize(st);
	return out;
}

/// The receiver type the visitors recorded for `caller`'s call, or "(none)".
/// Printed as a diagnostic: the whole S1 result follows from this one field.
std::string receiverType(sqlite3 *db, const char *caller)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db,
				 "SELECT rf.receiver_type FROM reference rf "
				 "JOIN entity c ON c.id = rf.caller_id "
				 "WHERE c.name = ? LIMIT 1",
				 -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, caller, -1, SQLITE_TRANSIENT);
	std::string out = "(none)";
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s && *s)
			out = s;
	}
	sqlite3_finalize(st);
	return out;
}

/// Number of call edges leaving `caller`.
long long outgoingCallEdges(sqlite3 *db, const char *caller)
{
	return scalarSql(db,
			 ("SELECT COUNT(*) FROM relation r JOIN entity se "
			  "ON se.id = r.source_id WHERE r.type = 1 AND se.name='" +
			  std::string(caller) + "'")
				 .c_str());
}

/// One language's expectations. The names are unique per language so one
/// project-wide database can hold all six fixtures without ambiguity, and the
/// S1 decoy is the sibling method that must never win.
///
/// `s1_gap` records whether S1 is a KNOWN unresolved case for this language
/// today, rather than an expectation of the language. Measured on this fixture:
/// the six visitors agree on the call fact itself (all six write
/// `reference(name='method', call_kind=1, receiver_text='o')`) but not on the
/// receiver's TYPE — `reference.receiver_type` is `*GoOwner` / `JavaOwner` /
/// `PyOwner` for Go, Java and Python, and EMPTY for C++, Rust and TypeScript,
/// whose visitors record neither a `receiver_type` on the call nor a
/// variable-type record to derive it from. Without that evidence the two
/// same-named methods cannot be told apart, so the resolver abstains and the
/// dependency is LOST (a false negative, not a wrong edge).
///
/// The assertions below treat the gap as a ratchet: the strong invariants must
/// hold for every language, the resolved set must be exactly the languages with
/// receiver-type evidence, and the moment a visitor starts recording it the
/// test fails on the count so the flag — and this comment — get updated instead
/// of the improvement passing unnoticed.
struct LanguageCase {
	const char *language;   // printed in the summary table
	const char *file;       // fixture path, relative to the project root
	const char *s1_caller;  // calls through a typed parameter
	const char *s1_owner;   // type whose method the call must reach
	const char *s1_method;  // that method's name (the decoy's name too)
	const char *s1_decoy;   // unrelated type declaring the same method name
	const char *s2_caller;  // calls a method nothing declares
	const char *s2_callee;  // the name that must NOT resolve
	bool s1_gap;            // S1 unresolved today: no receiver type recorded
};

} // namespace

int main()
{
	const std::string proj_dir = "/tmp/resolver_language_consistency";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir + "/src");

	// ── Go ────────────────────────────────────────────────────────────
	writeFile(proj_dir + "/src/lang.go",
		  "package p\n"
		  "\n"
		  "type GoOwner struct{}\n"
		  "\n"
		  "func (o *GoOwner) Method() int { return 1 }\n"
		  "\n"
		  "type GoDecoy struct{}\n"
		  "\n"
		  "func (d *GoDecoy) Method() int { return 2 }\n"
		  "\n"
		  "func goS1(o *GoOwner) int { return o.Method() }\n"
		  "\n"
		  "func goS2(u GoUndeclared) int { return u.computeAbsent() }\n");
	// ── C++ ───────────────────────────────────────────────────────────
	writeFile(proj_dir + "/src/lang.cpp",
		  "struct CppOwner {\n"
		  "\tint Method() { return 1; }\n"
		  "};\n"
		  "\n"
		  "struct CppDecoy {\n"
		  "\tint Method() { return 2; }\n"
		  "};\n"
		  "\n"
		  "int cppS1(CppOwner *o) { return o->Method(); }\n"
		  "\n"
		  "int cppS2(CppUndeclared u) { return u.computeAbsent(); }\n");
	// ── Java ──────────────────────────────────────────────────────────
	writeFile(proj_dir + "/src/Lang.java",
		  "class JavaOwner {\n"
		  "\tint method() { return 1; }\n"
		  "}\n"
		  "\n"
		  "class JavaDecoy {\n"
		  "\tint method() { return 2; }\n"
		  "}\n"
		  "\n"
		  "class JavaUse {\n"
		  "\tint javaS1(JavaOwner o) { return o.method(); }\n"
		  "\n"
		  "\tint javaS2(JavaUndeclared u) { return u.computeAbsent(); }\n"
		  "}\n");
	// ── Python ────────────────────────────────────────────────────────
	writeFile(proj_dir + "/src/lang.py",
		  "class PyOwner:\n"
		  "    def method(self):\n"
		  "        return 1\n"
		  "\n"
		  "\n"
		  "class PyDecoy:\n"
		  "    def method(self):\n"
		  "        return 2\n"
		  "\n"
		  "\n"
		  "def pyS1(o: PyOwner):\n"
		  "    return o.method()\n"
		  "\n"
		  "\n"
		  "def pyS2(u: PyUndeclared):\n"
		  "    return u.compute_absent()\n");
	// ── TypeScript ────────────────────────────────────────────────────
	writeFile(proj_dir + "/src/lang.ts",
		  "class TsOwner {\n"
		  "\tmethod(): number { return 1; }\n"
		  "}\n"
		  "\n"
		  "class TsDecoy {\n"
		  "\tmethod(): number { return 2; }\n"
		  "}\n"
		  "\n"
		  "function tsS1(o: TsOwner): number { return o.method(); }\n"
		  "\n"
		  "function tsS2(u: TsUndeclared): number { return u.computeAbsent(); }\n");
	// ── Rust ──────────────────────────────────────────────────────────
	writeFile(proj_dir + "/src/lang.rs",
		  "struct RsOwner;\n"
		  "\n"
		  "struct RsDecoy;\n"
		  "\n"
		  "impl RsOwner {\n"
		  "\tfn method(&self) -> i32 { 1 }\n"
		  "}\n"
		  "\n"
		  "impl RsDecoy {\n"
		  "\tfn method(&self) -> i32 { 2 }\n"
		  "}\n"
		  "\n"
		  "fn rsS1(o: &RsOwner) -> i32 { o.method() }\n"
		  "\n"
		  "fn rsS2(u: RsUndeclared) -> i32 { u.compute_absent() }\n");

	const std::vector<LanguageCase> cases = {
		{ "go", "src/lang.go", "goS1", "GoOwner", "Method", "GoDecoy",
		  "goS2", "computeAbsent", false },
		{ "c++", "src/lang.cpp", "cppS1", "CppOwner", "Method", "CppDecoy",
		  "cppS2", "computeAbsent", false },
		{ "java", "src/Lang.java", "javaS1", "JavaOwner", "method",
		  "JavaDecoy", "javaS2", "computeAbsent", false },
		{ "python", "src/lang.py", "pyS1", "PyOwner", "method", "PyDecoy",
		  "pyS2", "compute_absent", false },
		{ "typescript", "src/lang.ts", "tsS1", "TsOwner", "method",
		  "TsDecoy", "tsS2", "computeAbsent", false },
		{ "rust", "src/lang.rs", "rsS1", "RsOwner", "method", "RsDecoy",
		  "rsS2", "compute_absent", false },
	};

	const std::string db_path = proj_dir + "/consistency.db";
	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());

	g_engine = engine_create(db_path.c_str());
	CHECK(g_engine != nullptr);
	const uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
						   "language-consistency");
	CHECK(pid > 0);

	std::string request = "{\"paths\":[";
	for (size_t i = 0; i < cases.size(); ++i) {
		if (i)
			request += ",";
		request += "\"" + proj_dir + "/" + cases[i].file + "\"";
	}
	request += "]}";
	char *res = engine_index_files(g_engine, pid, request.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	// engine_destroy() joins the async knowledge builder before closing the
	// store, and every row asserted below is written synchronously by the
	// index path, so reading the database afterwards needs no sleep.
	engine_destroy(g_engine);
	g_engine = nullptr;

	sqlite3 *db = nullptr;
	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);

	printf("  %-11s %-12s %-14s %-14s %-12s\n", "language", "receiver_type",
	       "S1 -> owner", "S1 -> decoy", "S2 -> none");
	size_t s1_resolved = 0;
	for (const LanguageCase &c : cases) {
		// The fixture must really have parsed: without the caller entity the
		// assertions below would measure nothing.
		CHECK(scalarSql(db, ("SELECT COUNT(*) FROM entity WHERE name='" +
				     std::string(c.s1_caller) + "'").c_str()) == 1);
		CHECK(scalarSql(db, ("SELECT COUNT(*) FROM entity WHERE name='" +
				     std::string(c.s2_caller) + "'").c_str()) == 1);
		// Both methods of S1 must exist, or "no edge to the decoy" would hold
		// because the decoy was never recorded rather than because the
		// resolver rejected it. Counted by file and name rather than by
		// qualified_name: Rust and TypeScript record the bare method name with
		// no owner at all, so only the file and name are comparable.
		CHECK(scalarSql(db, ("SELECT COUNT(*) FROM entity WHERE kind IN (0,1) "
				     "AND name='" +
				     std::string(c.s1_method) +
				     "' AND file_path LIKE '%" + c.file + "'")
					.c_str()) == 2);

		// S2: nothing declares that receiver type or that method name, so
		// there is no evidence to resolve it with.
		const std::string absent =
			edgeKind(db, c.s2_caller, c.s2_callee);
		CHECK(absent == "(no edge)");

		std::string owner = "(no edge)";
		std::string decoy = "(no edge)";
		const long long outgoing = outgoingCallEdges(db, c.s1_caller);
		if (c.s1_gap) {
			// No receiver type reaches the resolver, so it must abstain
			// rather than pick one of the two same-named methods.
			CHECK(outgoing == 0);
		} else {
			// S1: the parameter's type owns the method...
			owner = edgeKindInType(db, c.s1_caller, c.s1_method,
					       c.s1_owner);
			// ...and the unrelated same-named method must not be reached.
			decoy = edgeKindInType(db, c.s1_caller, c.s1_method,
					       c.s1_decoy);
			CHECK(outgoing == 1);
			CHECK(owner != "(no edge)");
			// An edge to the decoy means the resolver preferred the name over
			// the receiver type the language recorded.
			CHECK(decoy == "(no edge)");
			++s1_resolved;
		}

		printf("  %-11s %-12s %-14s %-14s %-12s\n", c.language,
		       receiverType(db, c.s1_caller).c_str(), owner.c_str(),
		       decoy.c_str(), absent.c_str());
	}

	size_t expected_resolved = 0;
	for (const LanguageCase &c : cases)
		if (!c.s1_gap)
			++expected_resolved;
	CHECK(s1_resolved == expected_resolved);
	if (expected_resolved != cases.size()) {
		printf("  note: %zu of %zu languages resolve S1; the rest record no "
		       "receiver type (see the s1_gap comment)\n",
		       s1_resolved, cases.size());
	}

	sqlite3_close(db);
	// The fixture directory (sources and database) is left behind on failure so
	// the recorded qualified names and the missing edges can be inspected.
	if (checkFailures()) {
		fprintf(stderr,
			"=== resolver language consistency: FAILED ===\n"
			"    fixture kept at %s\n",
			proj_dir.c_str());
		return 1;
	}
	std::filesystem::remove_all(proj_dir);
	printf("=== resolver language consistency test passed ===\n");
	return 0;
}
