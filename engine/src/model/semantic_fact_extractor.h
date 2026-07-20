#ifndef CODESCOPE_MODEL_SEMANTIC_FACT_EXTRACTOR_H
#define CODESCOPE_MODEL_SEMANTIC_FACT_EXTRACTOR_H

#include <cstdint>
#include <string>
#include <vector>

#include "../store/store.h"

namespace model
{

/// SemanticFactExtractor scans semantic_records / entity / reference /
/// import for code patterns that imply a semantic primitive (mutex
/// lock, malloc, bare except, TODO comment, framework import, extern
/// "C"...) and persists each finding as one row in the semantic_fact
/// table. Facts are the input to the Phase 4 project_state snapshot.
///
/// The extractor is a stateless transformer: it does not own any data,
/// it only reads from the store and writes back. Construction is O(1);
/// extractAll() does all the work.
///
/// Usage (matches StateBuilder):
///   store::GraphStore *store = ...;
///   store->beginTransaction();
///   model::SemanticFactExtractor ex(store);
///   ex.extractAll(project_id);
///   store->commitTransaction();
///
/// Transaction model: extractAll does NOT begin/commit a transaction.
/// The caller wraps the entire run so that clear + re-extract is
/// atomic. Each extract* method calls store_->insertSemanticFacts()
/// once with the full batch — no per-symbol round-trip.
class SemanticFactExtractor {
    public:
	/// Construct with the store handle used for both reads (SELECT
	/// from semantic_records/entity/reference/import) and writes
	/// (INSERT into semantic_fact). The pointer is borrowed; the
	/// caller owns it and must keep it alive for the lifetime of
	/// the extractor.
	explicit SemanticFactExtractor(store::GraphStore *store)
		: store_(store)
	{
	}

	/// Run all extract phases in sequence for one project.
	/// Order: clear → sync → memory → error → pattern → framework
	/// → ffi. Each phase issues its own batched INSERT. The caller
	/// is responsible for BEGIN/COMMIT around the call.
	/// @param project_id  Project to extract facts for.
	/// @return Total number of facts inserted across all phases.
	int64_t extractAll(uint64_t project_id);

    private:
	/// Detect synchronization primitives:
	///   sync/mutex/lock, sync/mutex/unlock, sync/rwmutex/rlock,
	///   sync/atomic/load, sync/waitgroup/add, sync/defer.
	/// Source: semantic_records (kind=CallExpr) joined with the
	/// enclosing function in graph_nodes.
	int64_t extractSyncFacts(uint64_t project_id);

	/// Detect memory primitives:
	///   memory/cstring/alloc (C.CString/C.CBytes),
	///   memory/cstring/free  (C.free),
	///   memory/malloc/alloc  (malloc/calloc/realloc),
	///   memory/malloc/free   (free).
	int64_t extractMemoryFacts(uint64_t project_id);

	/// Detect error-handling anti-patterns:
	///   error/bare_except (Python `except:`),
	///   error/empty_catch (JS `catch {}`),
	///   error/ignored_return,
	///   error/unchecked_error.
	int64_t extractErrorFacts(uint64_t project_id);

	/// Detect code-smell patterns:
	///   pattern/todo, pattern/fixme (comment records),
	///   pattern/unwrap (Rust .unwrap() outside tests),
	///   pattern/panic (panic!/panic()),
	///   pattern/unsafe (Rust unsafe block).
	int64_t extractPatternFacts(uint64_t project_id);

	/// Detect framework adoption via the import table:
	///   framework/gin, framework/echo, framework/django,
	///   framework/express, framework/gorm.
	int64_t extractFrameworkFacts(uint64_t project_id);

	/// Detect FFI surface area:
	///   ffi/extern_call   (extern "C" / extern declaration),
	///   ffi/cgo_callback  (C.CF<callback> pattern, Go cgo),
	///   ffi/jni           (JNIEXPORT),
	///   ffi/wasm          (wasm_bindgen import).
	int64_t extractFfiFacts(uint64_t project_id);

	store::GraphStore *store_;
};

} // namespace model

#endif // CODESCOPE_MODEL_SEMANTIC_FACT_EXTRACTOR_H
