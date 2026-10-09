#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

// Engine instance state — TD-1 (docs/REVIEW_0.2.7.md), knife 3.
//
// History, so the shape below is not mistaken for an accident:
//
//   * Before TD-1 the engine kept three process-global `unique_ptr`
//     singletons (`g_store`, `g_query`, `g_parser`) declared in
//     engine_internal.h and defined in engine.cpp.
//   * Knife 1 moved them into one `EngineContext` object and kept the old
//     names as reference aliases, so no call site had to change.
//   * Knife 2 migrated all 340 call sites to `engineContext()` and turned the
//     object into a function-local static: no global object was constructed
//     before `main()` any more, but the instance was still reached for
//     implicitly.
//   * Knife 3 (this revision) deletes that accessor. The instance is created
//     by `engine_create()` (engine.h) and reaches engine code only as the
//     opaque `engine_t` handle every FFI entry point now takes as its first
//     parameter — the state is passed in, never looked up (code_rules §2:
//     "Avoid global/static mutable state unless strictly necessary").
//
// What is per-instance after knife 3: `store`, `query`, `parser` and their
// lifetime, which is exactly the state the FFI previously reached for.
//
// What is still process-wide, and therefore still a limit on running two
// *independent* engines in one process (recorded in the FFI contract in
// engine.h and in the CHANGELOG rather than left implicit):
//
//   * `verify::VerifierRegistry::instance()` — a process-wide registry bound
//     to (store, project_id) at construction. Pre-existing and already
//     documented as a v0.3 limitation inside engine_lifecycle.cpp.
//   * the async knowledge builder's thread/flags (async_knowledge.cpp) and the
//     shared-store lock it holds; one builder runs per process.
//   * `store::IndexProgress` and the parse-failure buffer in the store layer.
//
// The header deliberately FORWARD-DECLARES the member types and defines both
// special members out-of-line (engine_context.cpp), so a translation unit that
// only passes a handle through — e.g. store/store_parse_failure.cpp — can
// include it without pulling parser.h / query_engine.h / store.h into the
// build.

#include <memory>

// The `engine_t` handle type (engine.h). Including the public C header here is
// cheap (it pulls in stdint.h only) and keeps engineInstance() below typed
// exactly like the ABI.
#include "engine.h"

namespace store
{
class GraphStore;
}
namespace query
{
class QueryEngine;
}
class Parser;

/// One engine instance.
///
/// The C ABI exposes this type opaquely as `engine_t`
/// (`typedef struct CodescopeEngine *engine_t;` in engine.h): callers get a
/// handle from engine_create(), pass it to every stateful FFI function, and
/// release it with engine_destroy(). Engine code spells the same type
/// `EngineContext` through the alias below.
struct CodescopeEngine {
	/// Open graph store (SQLite). Non-null for the whole life of a
	/// successfully created instance; closed by engine_destroy().
	std::unique_ptr<store::GraphStore> store;
	/// Query engine bound to `store`.
	std::unique_ptr<query::QueryEngine> query;
	/// Parser with the statically linked grammars registered.
	std::unique_ptr<Parser> parser;

	/// Both special members are declared here and defined in
	/// engine_context.cpp. That is what lets this header keep its member types
	/// forward-declared: engine_create()/engine_destroy() construct and destroy
	/// the object in a translation unit that sees the concrete types, and a
	/// translation unit that only forwards a handle never instantiates the
	/// `unique_ptr` deleters (clang otherwise fails the deleter's
	/// incomplete-type static_assert).
	CodescopeEngine();
	~CodescopeEngine();
};

/// Internal spelling of the instance type. Every engine_*.cpp file uses this
/// name; the ABI sees the same type as `engine_t`.
using EngineContext = CodescopeEngine;

/// Resolve an ABI engine handle to the instance it names.
///
/// A null handle means the caller never created an instance, or already
/// destroyed it. Every FFI entry point funnels that case through its existing
/// "engine not initialized" guard (`if (!ctx || !ctx->store) ...`), so the
/// null contract is stated once here instead of being re-derived at 70 sites.
///
/// @param handle Handle returned by engine_create(); may be null.
/// @return The instance named by `handle`, or null when `handle` is null.
///
/// Ownership: borrowed — the instance is owned by the caller of
///            engine_create() and stays valid until engine_destroy(handle).
/// Lifetime:  the returned pointer must not outlive engine_destroy(handle).
/// Thread safety: none is taken here. The caller obeys the instance's
///            single-connection contract; see the thread-safety contract in
///            engine_internal.h.
inline EngineContext *engineInstance(engine_t handle)
{
	return handle;
}

#endif // ENGINE_CONTEXT_H
