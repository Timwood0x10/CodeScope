#include "engine_internal.h"
#include "platform_win.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <tree_sitter/api.h>

// ─── Lifecycle ─────────────────────────────────────────────────

int engine_init(const char *db_path)
{
	// Use unique_ptr for exception-safe initialization
	// If any constructor throws, previous allocations are auto-freed.
	g_store = std::make_unique<store::GraphStore>();
	if (!g_store->open(db_path)) {
		g_store.reset(); // Auto-cleanup via unique_ptr
		return -1;
	}
	g_query = std::make_unique<query::QueryEngine>(g_store.get());

	// Initialize parser and register available grammars
	g_parser = std::make_unique<Parser>();

	// Register all statically-linked tree-sitter grammars.
	// Grammars are compiled into the binary — no .so loading needed.
	const char *langs[] = { "python", "cpp",	"c",	      "rust",
				"swift",  "javascript", "typescript", "tsx",
				"go",	  "java" };
	for (auto lang : langs) {
		g_parser->registerLanguage(lang);
	}

	// Try to load sqlite-vec extension for vector embeddings (optional).
	{
		const char *gdir = getenv("GRAMMARS_DIR");
		std::string base = gdir ? gdir : "grammars";
		// Platform-specific vec0 extension suffix
		std::string vec_suffix;
#ifdef _WIN32
		vec_suffix = "/vec0.dll";
#elif __APPLE__
		vec_suffix = "/vec0.dylib";
#else
		vec_suffix = "/vec0.so";
#endif
		std::string vec_path = base + vec_suffix;
		sqlite3 *db = g_store->handle();
		char *ext_err = nullptr;
		int rc = SQLITE_ERROR;

		if (db) {
			// Attempt to load extension directly without dlopen pre-check
			rc = sqlite3_load_extension(db, vec_path.c_str(),
						    nullptr, &ext_err);
			if (rc != SQLITE_OK) {
				fprintf(stderr,
					"engine: sqlite-vec not available (%s)\n",
					ext_err ? ext_err : "unknown error");
			}
		}

		if (rc == SQLITE_OK) {
			char *sql_err = nullptr;
			sqlite3_exec(
				db,
				"CREATE VIRTUAL TABLE IF NOT EXISTS embeddings USING vec0("
				"    symbol_id INTEGER PRIMARY KEY,"
				"    vector FLOAT[384]"
				");",
				nullptr, nullptr, &sql_err);
			if (sql_err)
				sqlite3_free(sql_err);
			fprintf(stderr, "engine: sqlite-vec loaded\n");
		}
		if (ext_err)
			sqlite3_free(ext_err);
	}

	return 0;
}

void engine_shutdown()
{
	if (g_store) {
		g_store->close();
		g_store.reset();
	}
	g_query.reset();
	g_parser.reset();
}

// ─── Project ───────────────────────────────────────────────────

uint64_t engine_create_project(const char *root_path, const char *name)
{
	if (!g_store)
		return 0;
	return g_store->createProject(root_path, name);
}

uint64_t engine_get_latest_project_id()
{
	if (!g_store)
		return 0;
	return g_store->getLatestProjectId();
}
