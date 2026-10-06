#include "async_knowledge.h"
#include "engine_internal.h"
#include "platform_win.h"
#include "verify/registry.h"

#include <chrono>
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

// Create one engine instance (TD-1 knife 3): the state used to live in a
// process-global accessor, and the caller now owns it through the returned
// handle. Raw `new` is confined to this FFI boundary (code_rules §2): the
// unique_ptr keeps every failure path exception-safe, and the final release()
// IS the documented ownership transfer to the caller.
engine_t engine_create(const char *db_path)
{
	if (!db_path || !*db_path) {
		fprintf(stderr,
			"[module=ffi, method=engine_create] db_path is required\n");
		return nullptr;
	}
	std::unique_ptr<CodescopeEngine> engine;
	try {
		engine = std::make_unique<CodescopeEngine>();
	} catch (const std::exception &e) {
		fprintf(stderr, "[module=ffi, method=engine_create] %s\n",
			e.what());
		return nullptr;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_create] unknown exception\n");
		return nullptr;
	}

	EngineContext &ctx = *engine;
	try {
#ifdef HAVE_SQLITE_VEC
		// Register sqlite-vec once for the whole process. auto_extension fires on
		// every sqlite3_open, so the `vec0` module is available on the store's
		// connection by the time we create the `embeddings` table below.
		sqlite3_auto_extension(
			reinterpret_cast<void (*)(void)>(sqlite3_vec_init));
#endif

		// Use unique_ptr for exception-safe initialization
		// If any constructor throws, previous allocations are auto-freed.
		ctx.store = std::make_unique<store::GraphStore>();

		// Time GraphStore::open() (PRAGMAs + createSchema) so the
		// per-worker startup cost is visible in logs. createSchema runs
		// ~100+ CREATE TABLE/INDEX IF NOT EXISTS statements on a fresh
		// DB — significant in short-lived worker subprocesses.
		auto t_open_start = std::chrono::steady_clock::now();
		if (!ctx.store->open(db_path)) {
			fprintf(stderr,
				"engine_create: open failed: %s [module=engine, method=engine_create]\n",
				ctx.store ? ctx.store->error().c_str() :
					    "(null)");
			ctx.store.reset(); // Auto-cleanup via unique_ptr
			return nullptr;
		}
		auto t_open_end = std::chrono::steady_clock::now();
		fprintf(stderr,
			"engine_create: GraphStore::open=%lldms "
			"[module=engine, method=engine_create]\n",
			(long long)std::chrono::duration_cast<
				std::chrono::milliseconds>(t_open_end -
							   t_open_start)
				.count());

		ctx.query =
			std::make_unique<query::QueryEngine>(ctx.store.get());

		// Initialize parser and register available grammars
		ctx.parser = std::make_unique<Parser>();
		// Register all statically-linked tree-sitter grammars.
		// Grammars are compiled into the binary — no .so loading needed.
		const char *langs[] = { "python", "cpp",	"c",
					"rust",	  "javascript", "typescript",
					"tsx",	  "go",		"java" };
		for (auto lang : langs) {
			ctx.parser->registerLanguage(lang);
		}

		// sqlite-vec is statically compiled into the binary — no runtime
		// load_extension needed. The auto-extension registered above makes
		// vec0 available on every connection.
		// Note: vec0 embeddings table is no longer created — node_vectors
		// is the sole vector storage. searchSemantic reads node_vectors directly.

		return engine.release(); // ownership transfer to the caller
	} catch (const std::exception &e) {
		fprintf(stderr, "[module=ffi, method=engine_create] %s\n",
			e.what());
		return nullptr;
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_create] unknown exception\n");
		return nullptr;
	}
}

// Destroy one engine instance: the handle is consumed, and the destructor
// order is the one the managed state requires.
void engine_destroy(engine_t handle)
{
	// A null handle means engine_create() failed or already destroyed the
	// instance; destroying nothing is a no-op so callers do not need to guard.
	if (!handle)
		return;

	// Rebuild the owning unique_ptr: this is the ownership transfer back from
	// the caller (documented in engine.h), and it guarantees the instance is
	// released exactly once even if a step below throws.
	std::unique_ptr<CodescopeEngine> engine(handle);
	try {
		// Destruct in reverse construction order:
		//   constructed: store → query (depends on store) → parser (independent)
		//   destruct:    parser → query → store
		// This guarantees that the query engine's destructor (which may issue
		// SQLite calls) runs while the store is still alive, and the store is
		// closed last.

		// Clear the verifier registry BEFORE the store is torn down: every
		// registered Verifier holds a raw pointer to the store, so dropping
		// them first avoids any dangling-pointer access during store close.
		verify::VerifierRegistry::instance().clear();

		// Wait for the async knowledge builder to finish before destroying
		// the store. The builder thread dereferences it, so failing to join
		// here would cause a use-after-free.
		joinAsyncKnowledgeBuilder();

		engine->parser.reset(); // independent, safe to drop first
		// may issue SQLite work; drop before the store closes
		engine->query.reset();
		if (engine->store) {
			engine->store->close();
			engine->store.reset();
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "[module=ffi, method=engine_destroy] %s\n",
			e.what());
	} catch (...) {
		fprintf(stderr,
			"[module=ffi, method=engine_destroy] unknown exception\n");
	}
}

// ─── Project ───────────────────────────────────────────────────

uint64_t engine_create_project(engine_t handle, const char *root_path,
			       const char *name)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!root_path || !*root_path || !name || !*name) {
			fprintf(stderr,
				"[module=ffi, method=engine_create_project] root_path and name are required\n");
			return 0;
		}
		if (!ctx || !ctx->store)
			return 0;
		uint64_t pid = ctx->store->createProject(root_path, name);
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
			ctx->store.get(), pid);

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

uint64_t engine_get_latest_project_id(engine_t handle)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!ctx || !ctx->store)
			return 0;
		return ctx->store->getLatestProjectId();
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

uint64_t engine_get_project_id_by_path(engine_t handle, const char *root_path)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!root_path || !*root_path) {
			fprintf(stderr,
				"[module=ffi, method=engine_get_project_id_by_path] root_path is required\n");
			return 0;
		}
		if (!ctx || !ctx->store)
			return 0;
		return ctx->store->getProjectId(root_path);
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

uint64_t engine_get_project_node_count(engine_t handle, uint64_t project_id)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!ctx || !ctx->store)
			return 0;
		return ctx->store->getProjectNodeCount(project_id);
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
