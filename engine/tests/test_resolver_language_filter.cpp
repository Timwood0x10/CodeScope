// test_resolver_language_filter.cpp — regression tests for the resolver's
// language hard filter.
//
// Guards three defects fixed together:
//
//   1. The single-candidate fast path in ResolverPipeline::run() did not
//      apply the language hard filter that the main candidate loop applies.
//      A lone same-directory candidate in another language therefore
//      produced a cross-language CALLS edge with confidence 0.85
//      (a .cpp call site resolved to a .py entity).
//
//   2. The main loop compared raw language labels, but the path-based
//      classifier reports ".c" as "cpp" while the C visitor labels the
//      translation unit "c". Every legitimate C call site was rejected
//      whenever the fast path did not apply. C and C++ must be treated as
//      one language family.
//
//   3. The same mismatch in the JS/TS family: the path classifier reports
//      ".tsx" as "typescript" while the tsx visitor labels the entities it
//      defines "tsx", so a TSX call site could never resolve to a TSX
//      entity. Found on real projects rather than in theory: AIScope and
//      PolyScope both produced ZERO call edges (37 and 9 references
//      respectively named locally-defined symbols). javascript/typescript/
//      tsx form one family.
//
// Both scenarios are checked end-to-end: source files are written to disk,
// indexed through the FFI, and the resulting `relation` rows are inspected
// directly in SQLite.
//
// Boundary cases covered:
//   - a single cross-language candidate that shares the caller's directory
//     (must NOT produce an edge)
//   - a single cross-file same-language-family candidate with a forward
//     declaration in the caller's own file (must produce an edge)
//
// Exit code: 0 on success, 1 on any failed assertion.

#include "../include/engine.h"
#include "../src/resolver/factors.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

using namespace resolver;

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

/// Count CALLS (relation.type=1) rows, optionally filtered by the source
/// and target entity names. Empty filter strings mean "no filter".
static int countCalls(sqlite3 *db, uint64_t project_id, const char *source_name,
		      const char *target_name)
{
	const char *sql = "SELECT COUNT(*) FROM relation r "
			  "JOIN entity src ON src.id = r.source_id "
			  "JOIN entity tgt ON tgt.id = r.target_id "
			  "WHERE r.project_id=? AND r.type=1 "
			  "AND (? = '' OR src.name = ?) "
			  "AND (? = '' OR tgt.name = ?)";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare countCalls");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(st, 2, source_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, source_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, target_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, target_name, -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return count;
}

/// Count CALLS edges whose target lives in a file with the given suffix.
static int countCallsToFileSuffix(sqlite3 *db, uint64_t project_id,
				  const char *suffix)
{
	const char *sql = "SELECT COUNT(*) FROM relation r "
			  "JOIN entity tgt ON tgt.id = r.target_id "
			  "WHERE r.project_id=? AND r.type=1 "
			  "AND tgt.file_path LIKE ?";
	sqlite3_stmt *st = nullptr;
	check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK,
	      "prepare countCallsToFileSuffix");
	sqlite3_bind_int64(st, 1, static_cast<int64_t>(project_id));
	std::string pattern = std::string("%") + suffix;
	sqlite3_bind_text(st, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return count;
}

/// Write `content` to `path`, creating parent directories.
static void writeFile(const std::string &path, const char *content)
{
	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path());
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen fixture");
	fputs(content, f);
	fclose(f);
}

/// Index one directory and leave the engine open for SQL inspection.
static void indexDir(const char *db_path, const std::string &dir,
		     uint64_t *out_pid)
{
	unlink(db_path);
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(dir.c_str(), "lang-filter");
	check(pid > 0, "create_project");
	char *idx = engine_index_project(pid, dir.c_str(), nullptr);
	check(idx != nullptr, "index_project null");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);
	*out_pid = pid;
}

// ── Scenario 1: cross-language single candidate must be rejected ──
static void testCrossLanguageCandidateRejected()
{
	const std::string root = "/tmp/codescope_langfilter_mix";
	std::filesystem::remove_all(root);
	// The C++ caller and the Python candidate share one directory, so the
	// fast path's same_dir gate is satisfied — only the language rule can
	// reject it.
	writeFile(root + "/unit/caller.cpp", "void caller() { helper(); }\n");
	writeFile(root + "/unit/helper.py", "def helper():\n    return 1\n");

	const char *db_path = "/tmp/test_resolver_lang_filter_mix.db";
	uint64_t pid = 0;
	indexDir(db_path, root, &pid);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");
	int to_python = countCallsToFileSuffix(db, pid, ".py");
	int caller_calls = countCalls(db, pid, "caller", "");
	sqlite3_close(db);

	check(to_python == 0,
	      "cross-language edge: a C++ call site must not resolve to a "
	      "Python entity");
	check(caller_calls == 0,
	      "caller() must have no CALLS edges (its only candidate is "
	      "Python)");

	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("  [PASS] cross-language single candidate rejected\n");
}

// ── Scenario 2: C-family candidate must still resolve ──
static void testCFamilyCandidateAccepted()
{
	const std::string root = "/tmp/codescope_langfilter_cfam";
	std::filesystem::remove_all(root);
	// `main` in a.c calls helper(), which is defined in b.c. The caller's
	// language is classified "cpp" from the path while the entity is
	// labelled "c" — the C family rule must let this resolve.
	writeFile(root + "/src/a.c",
		  "void helper(void);\n"
		  "int main(void) { helper(); return 0; }\n");
	writeFile(root + "/src/b.c", "void helper(void) { }\n");

	const char *db_path = "/tmp/test_resolver_lang_filter_cfam.db";
	uint64_t pid = 0;
	indexDir(db_path, root, &pid);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");
	int main_to_helper = countCalls(db, pid, "main", "helper");
	sqlite3_close(db);

	check(main_to_helper >= 1,
	      "C call edge lost: main -> helper must resolve across files "
	      "inside the C family");

	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("  [PASS] C-family candidate accepted (main -> helper)\n");
}

// ── Scenario 3: the JS/TS family must resolve across .tsx files ──
//
// A `.tsx` call site is classified "typescript" from its path while the
// entity it calls is labelled "tsx" by the visitor, so the two labels never
// matched. Two real projects (AIScope, PolyScope) produced zero call edges
// because of it.
static void testJsTsFamilyCandidateAccepted()
{
	const std::string root = "/tmp/codescope_langfilter_jsts";
	std::filesystem::remove_all(root);
	writeFile(root + "/web/Widget.tsx",
		  "export function widgetHelper() { return 1; }\n");
	writeFile(root + "/web/App.tsx",
		  "import { widgetHelper } from './Widget';\n"
		  "export function App() { return widgetHelper(); }\n");

	const char *db_path = "/tmp/test_resolver_lang_filter_jsts.db";
	uint64_t pid = 0;
	indexDir(db_path, root, &pid);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");
	int app_to_helper = countCalls(db, pid, "App", "widgetHelper");
	int to_tsx = countCallsToFileSuffix(db, pid, ".tsx");
	sqlite3_close(db);

	check(app_to_helper >= 1,
	      "JS/TS family edge lost: a .tsx call site must resolve to a .tsx "
	      "entity (path label 'typescript' vs visitor label 'tsx')");
	check(to_tsx >= 1, "no CALLS edge into the .tsx file at all");

	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("  [PASS] JS/TS family candidate accepted (App -> widgetHelper)\n");
}

// ── The relative-import matcher itself ──
//
// This is the pure part of the ImportModuleMatch factor, and it is tested
// directly rather than through a fixture because the fixture would pass for the
// wrong reason: with the current `import` table the factor cannot fire at all.
// The table records the module request in `target_path` (`../lib/Widget`) but
// its `alias` column holds the module's LAST SEGMENT, not the name the module
// was bound to, so a bare call to `widgetHelper` cannot be linked back to its
// module. Tested here: the matcher, which is what the factor will use once that
// link exists.
static void testRelativeImportMatcher()
{
	using resolver::relativeImportMatchesFile;
	const std::string dir = "/p/app/src";
	check(relativeImportMatchesFile(dir, "./helper",
					"/p/app/src/helper.ts"),
	      "same-directory ./helper");
	check(relativeImportMatchesFile(dir, "../lib/Widget",
					"/p/app/lib/Widget.tsx"),
	      "../lib/Widget resolves to the .tsx file");
	check(relativeImportMatchesFile(dir, "../lib/Widget",
					"/p/app/lib/Widget.ts"),
	      "the match is extension-agnostic");
	check(relativeImportMatchesFile(dir, "./utils/index.ts",
					"/p/app/src/utils/index.ts"),
	      "an explicit file specifier");
	check(relativeImportMatchesFile(dir, "./utils",
					"/p/app/src/utils/index.ts"),
	      "./utils may name a directory entry point");
	// Negative controls: anything that names no project file must not match,
	// or the factor would invent evidence.
	check(!relativeImportMatchesFile(dir, "react", "/p/app/src/react.ts"),
	      "a bare package name is never path evidence");
	check(!relativeImportMatchesFile(dir, "@/lib/utils",
					 "/p/app/src/lib/utils.ts"),
	      "a path ALIAS is not resolvable without tsconfig paths");
	check(!relativeImportMatchesFile(dir, "../other/Decoy",
					 "/p/app/lib/Widget.tsx"),
	      "a different module must not match");
	// With an unknown caller directory a relative specifier cannot be turned
	// into a path at all: answering yes would be a guess.
	check(!relativeImportMatchesFile("", "./helper", "/p/helper.ts"),
	      "no caller directory means no path evidence");
	printf("  [PASS] relative import matcher (5 positive, 4 negative)\n");
}

// ── Scenario 4: a RELATIVE import must beat a same-named decoy ──
//
// The end-to-end half of the ImportModuleMatch evidence (the matcher itself is
// tested above). TWO candidates share the name in different directories, so
// this measures the import evidence rather than "a lone candidate wins on any
// positive score" — the first version of this scenario had one candidate and
// stayed green even with the factor disabled, i.e. it proved nothing.
static void testRelativeImportBeatsDecoy()
{
	const std::string root = "/tmp/codescope_langfilter_importpath";
	std::filesystem::remove_all(root);
	// The decoy lives in `app/decoy/`, which sorts BEFORE `app/lib/`: entity
	// ids follow the scan order, and the resolver keeps the first candidate
	// when scores are equal (`c.total_score > best_score` is a strict
	// comparison). So without the import evidence the decoy wins, which is
	// what makes this scenario measure the evidence rather than the tie-break.
	// Two earlier versions of this fixture passed with the evidence disabled —
	// one had a single candidate, the other had a decoy that sorted last.
	// The decoy sits in `app/decoy/lib/` so the module's last segment ("lib")
	// appears in BOTH candidates' paths: the pre-existing import heuristic
	// matches on that segment and is therefore tied, while resolving the
	// specifier to a path picks exactly one. Its directory also sorts first, so
	// a tie would hand the edge to the decoy.
	writeFile(root + "/app/decoy/lib/Decoy.tsx",
		  "export function widgetHelper() { return 2; }\n");
	writeFile(root + "/app/lib/Widget.tsx",
		  "export function widgetHelper() { return 1; }\n");
	// A DIRECTORY import on purpose: the module's last segment ("lib") matches
	// no file stem, so the pre-existing heuristic that compares a candidate's
	// file name with the import's last segment (`../lib/Widget` → `Widget`)
	// cannot resolve this — only matching the specifier against the candidate's
	// PATH can, which is what ImportModuleMatch does. With `../lib/Widget` as
	// the specifier this scenario passed even with the new evidence disabled.
	writeFile(root + "/app/src/App.tsx",
		  "import { widgetHelper } from '../lib';\n"
		  "export function App() { return widgetHelper(); }\n");

	const char *db_path = "/tmp/test_resolver_lang_filter_importpath.db";
	uint64_t pid = 0;
	indexDir(db_path, root, &pid);

	sqlite3 *db = nullptr;
	check(sqlite3_open(db_path, &db) == SQLITE_OK, "sqlite_open");
	int to_imported = countCallsToFileSuffix(db, pid, "lib/Widget.tsx");
	int to_decoy = countCallsToFileSuffix(db, pid, "decoy/lib/Decoy.tsx");
	int app_calls_helper = countCalls(db, pid, "App", "widgetHelper");
	sqlite3_close(db);

	check(to_imported >= 1,
	      "the relative import must win over a same-named decoy in another "
	      "directory: App() imports widgetHelper from ../lib/Widget");
	check(to_decoy == 0, "the decoy definition must not be chosen");
	check(app_calls_helper == 1, "exactly one widgetHelper call edge");

	engine_shutdown();
	std::filesystem::remove_all(root);
	printf("  [PASS] relative import beats a same-named decoy\n");
}

// ── The family matrix, including the pairs that must stay incompatible ──
static void testLanguageFamilyMatrix()
{
	check(languagesCompatible("c", "cpp"), "C family");
	check(languagesCompatible("javascript", "typescript"), "js/ts");
	check(languagesCompatible("typescript", "tsx"), "ts/tsx");
	check(languagesCompatible("javascript", "tsx"), "js/tsx");
	// Negative controls: unrelated languages must remain incompatible, or
	// the filter stops being a filter.
	check(!languagesCompatible("python", "rust"), "python/rust");
	check(!languagesCompatible("python", "typescript"), "python/ts");
	check(!languagesCompatible("go", "typescript"), "go/ts");
	check(!languagesCompatible("rust", "tsx"), "rust/tsx");
	check(!languagesCompatible("java", "javascript"), "java/js");
	// An unknown label never filters anything.
	check(languagesCompatible("", "rust"), "empty is always compatible");
	printf("  [PASS] language family matrix (4 compatible, 5 rejected)\n");
}

int main()
{
	printf("=== resolver language filter regression tests ===\n");
	testCrossLanguageCandidateRejected();
	testCFamilyCandidateAccepted();
	testJsTsFamilyCandidateAccepted();
	testRelativeImportMatcher();
	testRelativeImportBeatsDecoy();
	testLanguageFamilyMatrix();
	printf("\nAll resolver language filter tests passed.\n");
	return 0;
}
