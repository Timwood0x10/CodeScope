#include "async_knowledge.h"
#include "engine_internal.h"
#include "platform_win.h"
#include "verify/registry.h"

#include <cstdio>
#include <cstdlib>
#include <sqlite3.h>
#include <tree_sitter/api.h>

// sqlite-vec is compiled directly into this binary (see engine/CMakeLists.txt).
// When present, register its `vec0` virtual-table module on every new SQLite
// connection so the `embeddings` table can be created without a runtime
// load_extension() call. Guarded by HAVE_SQLITE_VEC so the build still links
// when the amalgamation was unavailable at configure time.
#ifdef HAVE_SQLITE_VEC
extern "C" int sqlite3_vec_init(sqlite3 *db, char **pzErrMsg,
				const sqlite3_api_routines *pApi);
#endif

// ─── Lifecycle ─────────────────────────────────────────────────

int engine_init(const char *db_path)
{
	try {
		if (!db_path || !*db_path) {
			fprintf(stderr,
				"[module=ffi, method=engine_init] db_path is required\n");
			return -1;
		}
#ifdef HAVE_SQLITE_VEC
		// Register sqlite-vec once for the whole process. auto_extension fires on
		// every sqlite3_open, so the `vec0` module is available on the store's
		// connection by the time we create the `embeddings` table below.
		sqlite3_auto_extension(
			reinterpret_cast<void (*)(void)>(sqlite3_vec_init));
#endif

		// Use unique_ptr for exception-safe initialization
		// If any constructor throws, previous allocations are auto-freed.
		g_store = std::make_unique<store::GraphStore>();

		if (!g_store->open(db_path)) {
			fprintf(stderr,
				"engine_init: open failed: %s [module=engine, method=engine_init]\n",
				g_store ? g_store->error().c_str() : "(null)");
			g_store.reset(); // Auto-cleanup via unique_ptr
			return -1;
		}
		// Initialize LadybugDB for graph storage (non-fatal if unavailable)
		g_store->initLadybugDB();
		g_query = std::make_unique<query::QueryEngine>(g_store.get());

		// Initialize parser and register available grammars
		g_parser = std::make_unique<Parser>();
		// Register all statically-linked tree-sitter grammars.
		// Grammars are compiled into the binary — no .so loading needed.
		const char *langs[] = { "python", "cpp",	"c",
					"rust",	  "javascript", "typescript",
					"tsx",	  "go",		"java" };
		for (auto lang : langs) {
			g_parser->registerLanguage(lang);
		}

		// sqlite-vec is statically compiled into the binary — no runtime
		// load_extension needed. The auto-extension registered above makes
		// vec0 available on every connection.
		// Note: vec0 embeddings table is no longer created — node_vectors
		// is the sole vector storage. searchSemantic reads node_vectors directly.

		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "[module=ffi, method=engine_init] %s\n",
			e.what());
		return -1;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_init] unknown exception\n");
		return -1;
	}
}

void engine_shutdown()
{
	try {
		// Destruct in reverse construction order:
		//   constructed: g_store → g_query (depends on store) → g_parser (independent)
		//   destruct:    g_parser → g_query → g_store
		// This guarantees that g_query's destructor (which may issue SQLite calls)
		// runs while g_store is still alive, and g_store is closed last.

		// Clear the verifier registry BEFORE g_store is torn down: every
		// registered Verifier holds a raw pointer to g_store, so dropping
		// them first avoids any dangling-pointer access during store close.
		verify::VerifierRegistry::instance().clear();

		// Wait for the async knowledge builder to finish before destroying
		// g_store. The builder thread dereferences g_store, so failing to
		// join here would cause a use-after-free.
		joinAsyncKnowledgeBuilder();

		g_parser.reset(); // independent, safe to drop first
		g_query.reset(); // may do SQLite work via g_store, destruct BEFORE store closes
		if (g_store) {
			g_store->closeLadybugDB();
			g_store->close();
			g_store.reset();
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "[module=ffi, method=engine_shutdown] %s\n",
			e.what());
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_shutdown] unknown exception\n");
	}
}

// ─── Project ───────────────────────────────────────────────────

uint64_t engine_create_project(const char *root_path, const char *name)
{
	try {
		if (!root_path || !*root_path || !name || !*name) {
			fprintf(stderr,
				"[module=ffi, method=engine_create_project] root_path and name are required\n");
			return 0;
		}
		if (!g_store)
			return 0;
		uint64_t pid = g_store->createProject(root_path, name);
		if (pid == 0)
			return 0;

		// Register the default verifier set for this project. The registry
		// is a process-wide singleton bound to (store, project_id) at
		// construction; clear() first so verifiers from a previous project
		// (e.g. a re-index creating a fresh project row) are not left behind
		// to receive dispatched claims for the wrong project. For genuine
		// multi-project workflows this is a known v0.3 limitation — a
		// future revision should key the registry by project_id.
		verify::VerifierRegistry::instance().clear();
		verify::VerifierRegistry::instance().register_default_verifiers(
			g_store.get(), pid);

		return pid;
	} catch (const std::exception &e) {
		fprintf(stderr,
			"[module=ffi, method=engine_create_project] %s\n",
			e.what());
		return 0;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_create_project] unknown exception\n");
		return 0;
	}
}

uint64_t engine_get_latest_project_id()
{
	try {
		if (!g_store)
			return 0;
		return g_store->getLatestProjectId();
	} catch (const std::exception &e) {
		fprintf(stderr,
			"[module=ffi, method=engine_get_latest_project_id] %s\n",
			e.what());
		return 0;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_get_latest_project_id] unknown exception\n");
		return 0;
	}
}

uint64_t engine_get_project_id_by_path(const char *root_path)
{
	try {
		if (!root_path || !*root_path) {
			fprintf(stderr,
				"[module=ffi, method=engine_get_project_id_by_path] root_path is required\n");
			return 0;
		}
		if (!g_store)
			return 0;
		return g_store->getProjectId(root_path);
	} catch (const std::exception &e) {
		fprintf(stderr,
			"[module=ffi, method=engine_get_project_id_by_path] %s\n",
			e.what());
		return 0;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_get_project_id_by_path] unknown exception\n");
		return 0;
	}
}

uint64_t engine_get_project_node_count(uint64_t project_id)
{
	try {
		if (!g_store)
			return 0;
		return g_store->getProjectNodeCount(project_id);
	} catch (const std::exception &e) {
		fprintf(stderr,
			"[module=ffi, method=engine_get_project_node_count] %s\n",
			e.what());
		return 0;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_get_project_node_count] unknown exception\n");
		return 0;
	}
}
