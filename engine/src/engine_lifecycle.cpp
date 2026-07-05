#include "engine_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Lifecycle ─────────────────────────────────────────────────

int engine_init(const char *db_path)
{
	g_store = new store::GraphStore();
	if (!g_store->open(db_path)) {
		delete g_store;
		g_store = nullptr;
		return -1;
	}
	g_query = new query::QueryEngine(g_store);

	// Initialize parser and register available grammars
	g_parser = new Parser();

	// Try to register grammars from the grammars/ directory
	// The directory is resolved relative to the binary or from GRAMMARS_DIR env
	const char *grammars_dir = getenv("GRAMMARS_DIR");
	std::string base = grammars_dir ? grammars_dir : "grammars";

	// Language → grammar .so path mapping
	const char *langs[] = { "python", "cpp",	"c",	      "rust",
				"swift",  "javascript", "typescript", "tsx",
				"go",	  "java" };
	for (auto lang : langs) {
		std::string path = base + "/tree-sitter-" + lang + ".so";
		g_parser->registerLanguage(lang, path.c_str());
	}

	// Try to load sqlite-vec extension for vector embeddings (optional).
	{
		std::string vec_path = base + "/vec0.dylib";
		sqlite3 *db = g_store->handle();
		char *ext_err = nullptr;
		int rc = SQLITE_ERROR;

		if (db) {
			void *handle = dlopen(vec_path.c_str(),
					      RTLD_LAZY | RTLD_LOCAL);
			if (handle) {
				dlclose(handle);
				rc = sqlite3_load_extension(db,
							    vec_path.c_str(),
							    nullptr, &ext_err);
			} else {
				fprintf(stderr,
					"engine: sqlite-vec not available (%s)\n",
					dlerror());
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
		} else if (rc != SQLITE_OK && ext_err) {
			fprintf(stderr,
				"engine: sqlite-vec not available: %s\n",
				ext_err);
		}
		if (ext_err)
			sqlite3_free(ext_err);
	}

	return 0;
}

void engine_shutdown()
{
	delete g_query;
	g_query = nullptr;

	delete g_parser;
	g_parser = nullptr;

	if (g_store) {
		g_store->close();
		delete g_store;
		g_store = nullptr;
	}
}

// ─── Project ───────────────────────────────────────────────────

uint64_t engine_create_project(const char *root_path, const char *name)
{
	if (!g_store)
		return 0;
	return g_store->createProject(root_path, name);
}
