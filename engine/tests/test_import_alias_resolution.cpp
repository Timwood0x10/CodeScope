// test_import_alias_resolution — `import { x as y }` must resolve to x.
//
// Defect guarded: a TypeScript/JavaScript aliased import binds a LOCAL name to
// an imported symbol, and only the local name appears at the call site. The
// visitor recorded the module binding (`emitImportBinding(y, "../lib/helper")`)
// but not which symbol `y` stands for, so a bare `y()` named no declaration at
// all and produced NO edge — a silent false negative for a shape every
// TypeScript codebase uses (`import { useState as useLocalState }`).
//
// The call is now emitted under the imported symbol, so it resolves like any
// other call to that symbol. The fixture puts the same symbol name in two
// modules on purpose: the call must reach the module it was imported FROM, which
// makes this the case the ImportModuleMatch factor was written for, and the
// assertion fails if the resolver guesses the other one.
#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

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

/// The file of the single callee `caller` reaches, or "(no edge)".
std::string calleeFile(sqlite3 *db, const char *caller)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(
		      db,
		      "SELECT te.file_path FROM relation r "
		      "JOIN entity se ON se.id = r.source_id "
		      "JOIN entity te ON te.id = r.target_id "
		      "WHERE r.type = 1 AND se.name = ?",
		      -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, caller, -1, SQLITE_TRANSIENT);
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

/// The callee name recorded for `caller`'s call site, or "(none)".
std::string referenceName(sqlite3 *db, const char *caller)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(
		      db,
		      "SELECT rf.name FROM reference rf JOIN entity e "
		      "ON e.id = rf.caller_id WHERE e.name = ? LIMIT 1",
		      -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, caller, -1, SQLITE_TRANSIENT);
	std::string out = "(none)";
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s)
			out = s;
	}
	sqlite3_finalize(st);
	return out;
}

} // namespace

int main()
{
	const std::string proj_dir = "/tmp/import_alias_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir + "/src/lib");
	std::filesystem::create_directories(proj_dir + "/src/util");

	// The same exported symbol in two modules: the import decides which one the
	// call means, so a resolver that ignores the import has a 50% chance of
	// being wrong rather than merely unresolved.
	writeFile(proj_dir + "/src/lib/helper.ts",
		  "export function pick(): number { return 1; }\n");
	writeFile(proj_dir + "/src/util/helper.ts",
		  "export function pick(): number { return 2; }\n");
	writeFile(proj_dir + "/src/use.ts",
		  "import { pick as pickLib } from '../lib/helper';\n"
		  "\n"
		  "export function use(): number { return pickLib(); }\n");

	const std::string db_path = proj_dir + "/alias.db";
	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());

	g_engine = engine_create(db_path.c_str());
	CHECK(g_engine != nullptr);
	const uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
						   "import-alias");
	CHECK(pid > 0);
	const std::string request = "{\"paths\":[\"" + proj_dir +
				    "/src/lib/helper.ts\",\"" + proj_dir +
				    "/src/util/helper.ts\",\"" + proj_dir +
				    "/src/use.ts\"]}";
	char *res = engine_index_files(g_engine, pid, request.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	// engine_destroy() joins the async knowledge builder before closing the
	// store, and every row asserted below is written synchronously by the index
	// path, so reading the database afterwards needs no sleep.
	engine_destroy(g_engine);
	g_engine = nullptr;

	sqlite3 *db = nullptr;
	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);

	// The fixture must really have parsed, and the ambiguity must exist: two
	// same-named functions in different files.
	CHECK(scalarSql(db, "SELECT COUNT(*) FROM entity WHERE name='pick'") == 2);
	CHECK(scalarSql(db, "SELECT COUNT(*) FROM entity WHERE name='use'") == 1);

	// The call site is recorded as the IMPORTED symbol, not the local alias: the
	// alias names nothing that can be declared, which is why the edge was lost.
	CHECK(referenceName(db, "use") == "pick");

	// One edge, to the module the symbol was imported from.
	CHECK(calleeFile(db, "use") == proj_dir + "/src/lib/helper.ts");

	sqlite3_close(db);
	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());
	if (checkFailures()) {
		fprintf(stderr, "=== import alias resolution: FAILED ===\n");
		return 1;
	}
	std::filesystem::remove_all(proj_dir);
	printf("=== import alias resolution test passed ===\n");
	return 0;
}
