#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

// Engine state container — TD-1 (REVIEW_0.2.7.md), knives 1 and 2.
//
// The engine used to keep three process-global `unique_ptr` singletons
// (`g_store`, `g_query`, `g_parser`), declared in engine_internal.h and defined
// in engine.cpp. TD-1 replaces that with one explicit context object so the
// state can eventually be threaded through the FFI boundary as a handle
// instead of being reached for globally (code_rules §2: "Avoid global/static
// mutable state unless strictly necessary").
//
//   * knife 1 (seam, behaviour unchanged) — introduce `EngineContext` and keep
//     the old names as reference aliases so no call site had to change.
//   * knife 2 (this commit) — migrate all 340 internal call sites to
//     `engineContext().store` / `.query` / `.parser`, delete the aliases, and
//     turn the namespace-scope instance into a function-local static, so there
//     is no global object left to reach for.
//   * knife 3 (pending) — replace the accessor with a handle passed in from the
//     FFI entry points, so two engine instances can coexist in one process.
//
// Each knife was verified with a byte-identical differential tool matrix (43
// tools against a fixed DB) in addition to the test suite.
//
// Lifecycle and thread safety are unchanged from the previous singletons:
//   * engine_init() fills the members, engine_shutdown() clears them.
//   * The Rust MCP server calls FFI functions sequentially from a single
//     thread; no lock is taken here. See engine_internal.h for the full
//     thread-safety contract.
//
// The header deliberately FORWARD-DECLARES the member types and defines both
// special members out-of-line (engine_context.cpp), so a translation unit that
// only touches the store — e.g. store/store_parse_failure.cpp — can include it
// without pulling parser.h / query_engine.h / store.h into the build.

#include <cstdint>
#include <memory>

namespace store
{
class GraphStore;
}
namespace query
{
class QueryEngine;
}
class Parser;

/// The engine's process-wide state.
struct EngineContext {
	/// Open graph store (SQLite); non-null between engine_init and engine_shutdown.
	std::unique_ptr<store::GraphStore> store;
	/// Query engine bound to `store`.
	std::unique_ptr<query::QueryEngine> query;
	/// Parser with the statically linked grammars registered.
	std::unique_ptr<Parser> parser;
	/// Project the current FFI call targets. Reserved for knife 3, where it
	/// becomes part of the handle instead of being re-derived per call.
	uint64_t project_id = 0;

	/// Both special members are declared here and defined in
	/// engine_context.cpp. That is what lets this header keep its member types
	/// forward-declared: the function-local static in engineContext() needs a
	/// constructor and a destructor, and neither may be instantiated in a TU
	/// that never sees the concrete types (clang otherwise fails the
	/// `unique_ptr` deleter's incomplete-type static_assert while generating
	/// the static's exception cleanup).
	EngineContext();
	~EngineContext();
};

/// The engine state accessor — the only way engine code reaches the state.
///
/// Knife 2 migrated all 340 internal call sites from the former `engineContext().store` /
/// `engineContext().query` / `engineContext().parser` globals to `engineContext().store` / `.query` /
/// `.parser`, and the aliases are gone, so a stale call site is now a compile
/// error rather than a silent second path to the same object.
///
/// The instance is a function-local static rather than a namespace-scope
/// global: nothing is constructed before `main` and no global object is
/// exported, which is the "去全局" half of TD-1. Knife 3 completes it by
/// replacing this accessor with a handle passed in from the FFI boundary, so
/// two engine instances can coexist in one process.
///
/// Thread-safe by the language rule for function-local statics; `engine_init`
/// fills the members and `engine_shutdown` clears them, both called from the
/// server's single dispatch thread.
inline EngineContext &engineContext()
{
	static EngineContext context;
	return context;
}

#endif // ENGINE_CONTEXT_H
