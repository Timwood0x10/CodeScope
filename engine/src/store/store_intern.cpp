#include "store.h"

namespace store
{

// ================================================================
// Phase 3: Call Edge Resolution
// ================================================================
//
// buildCallEdgesSQL was the original SQL-JOIN-based call edge resolver
// (P1 intra-file / P3 name-based / P3b short-name). It is fully removed
// as dead code: buildGraph() casts build_calls to void (store_graph.cpp)
// and routes all call-edge construction through the Resolver Pipeline
// (engine/src/resolver/pipeline.cpp, Phase 1.3) with multi-factor scoring.
// See docs/bugs/bug_resolve_strategy.zh.md Bug 1 for the removal rationale.

} // namespace store
