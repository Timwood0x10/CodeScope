// test_js_ts_call_facts.cpp — regression test for structured call facts in
// the JavaScript / TypeScript visitors.
//
// Defect guarded: JsVisitor::visitCallExpr never called
// SemanticEmitter::setCallFacts(), while the Go / Python / C / Rust / Java
// visitors did. JS/TS reference rows therefore carried empty
// receiver_type / qualified_target / import_alias evidence, which
//   * disabled the resolver's fuzzy fallback (its has_evidence gate
//     requires at least one structured field), and
//   * left factorReceiverTypeMatch neutral for every candidate, so
//     same-name methods could not be disambiguated.
//
// The test indexes a small JS + TS project and inspects the persisted
// semantic_records rows for the call sites.
//
// Boundary cases covered:
//   * `this.method()` → receiver_type must be the enclosing class name
//   * `ns.fn()` on a namespace import → import_alias must be the alias
//   * `obj.method()` → receiver_text/qualified_target must name the object
//   * a BARE call must leave every structured field empty (the visitor
//     must not fabricate a receiver for a direct call)
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

/// The four structured call-fact columns of one CallExpr row.
struct CallFact {
	std::string qualified_target;
	std::string receiver_text;
	std::string receiver_type;
	std::string import_alias;
	bool found = false;
};

/// Fetch the CallExpr (kind=9) row named `name` whose file ends with
/// `file_suffix`. Returns found=false when no such row exists.
static CallFact getCallFact(sqlite3 *db, uint64_t project_id, const char *name,
			    const char *file_suffix)
{
	const char *sql =
		"SELECT qualified_target, receiver_text, receiver_type, "
		"import_alias FROM semantic_records "
		"WHERE project_id=? AND kind=9 AND name=? AND file_path LIKE ? "
		"ORDER BY rowid LIMIT 1";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare getCallFact");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
	std::string pattern = std::string("%") + file_suffix;
	sqlite3_bind_text(st, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
	CallFact fact;
	if (sqlite3_step(st) == SQLITE_ROW) {
		auto text = [&](int col) -> std::string {
			const char *t = reinterpret_cast<const char *>(
				sqlite3_column_text(st, col));
			return t ? t : "";
		};
		fact.qualified_target = text(0);
		fact.receiver_text = text(1);
		fact.receiver_type = text(2);
		fact.import_alias = text(3);
		fact.found = true;
	}
	sqlite3_finalize(st);
	return fact;
}

static void writeFile(const std::string &path, const char *content)
{
	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path());
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen fixture");
	fputs(content, f);
	fclose(f);
}

int main()
{
	const std::string root = "/tmp/codescope_js_ts_call_facts";
	std::filesystem::remove_all(root);

	// util.js — namespace-import target.
	writeFile(root + "/src/util.js",
		  "export function greet() { return 'hi'; }\n");
	// main.js — namespace call, object call, and a bare call.
	writeFile(root + "/src/main.js",
		  "import * as util from './util.js';\n"
		  "class Renderer { render() { return 1; } }\n"
		  "export function standalone() { return 0; }\n"
		  "export function main() {\n"
		  "  const r = new Renderer();\n"
		  "  r.render();\n"
		  "  util.greet();\n"
		  "  standalone();\n"
		  "}\n");
	// timeline.ts — `this.method()` receiver inference.
	writeFile(root + "/src/timeline.ts",
		  "export class Timeline {\n"
		  "  render(): number { return 1; }\n"
		  "  draw(): number { return this.render(); }\n"
		  "}\n");

	const char *db_path = "/tmp/test_js_ts_call_facts.db";
	unlink(db_path);
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(root.c_str(), "js-ts-call-facts");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, root.c_str(), nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");

	// ── Case 1: namespace import call `util.greet()` ──────────────
	{
		CallFact f = getCallFact(db, pid, "greet", "main.js");
		check(f.found, "util.greet() call record missing");
		check(f.qualified_target == "util.greet",
		      "util.greet() qualified_target must be \"util.greet\"");
		check(f.receiver_text == "util",
		      "util.greet() receiver_text must be \"util\"");
		check(f.import_alias == "util",
		      "util.greet() import_alias must be \"util\"");
		printf("  [PASS] util.greet(): qualified=%s alias=%s\n",
		       f.qualified_target.c_str(), f.import_alias.c_str());
	}

	// ── Case 2: object call `r.render()` ──────────────────────────
	{
		CallFact f = getCallFact(db, pid, "render", "main.js");
		check(f.found, "r.render() call record missing");
		check(f.receiver_text == "r",
		      "r.render() receiver_text must be \"r\"");
		check(f.qualified_target == "r.render",
		      "r.render() qualified_target must be \"r.render\"");
		printf("  [PASS] r.render(): receiver=%s qualified=%s\n",
		       f.receiver_text.c_str(), f.qualified_target.c_str());
	}

	// ── Case 3: TS `this.render()` → receiver_type = class ────────
	{
		CallFact f = getCallFact(db, pid, "render", "timeline.ts");
		check(f.found, "this.render() call record missing");
		check(f.receiver_text == "this",
		      "this.render() receiver_text must be \"this\"");
		check(f.receiver_type == "Timeline",
		      "this.render() receiver_type must be the enclosing class "
		      "\"Timeline\"");
		printf("  [PASS] this.render(): receiver=%s type=%s\n",
		       f.receiver_text.c_str(), f.receiver_type.c_str());
	}

	// ── Case 4: bare call must not fabricate a receiver ───────────
	{
		CallFact f = getCallFact(db, pid, "standalone", "main.js");
		check(f.found, "standalone() call record missing");
		check(f.qualified_target.empty(),
		      "bare call must not set qualified_target");
		check(f.receiver_text.empty(),
		      "bare call must not set receiver_text");
		check(f.receiver_type.empty(),
		      "bare call must not set receiver_type");
		check(f.import_alias.empty(),
		      "bare call must not set import_alias");
		printf("  [PASS] standalone(): no fabricated receiver\n");
	}

	sqlite3_close(db);
	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("\nAll JS/TS call-fact tests passed.\n");
	return 0;
}
