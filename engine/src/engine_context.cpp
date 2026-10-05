#include "engine_context.h"

// The concrete member types are only needed here: engine_context.h
// forward-declares them so that TUs which merely touch the store do not pay for
// these includes (see the header's comment).
#include "parser/parser.h"
#include "query/query_engine.h"
#include "store/store.h"

// Completes the two out-of-line special members declared in the header.
// Instantiating the unique_ptr constructors/deleters here is exactly why the
// header can keep its member types forward-declared.
//
// This TU holds no state: knife 2 moved the single instance into the
// function-local static in engineContext(), so there is no namespace-scope
// object to construct before main(). The file is kept for these definitions
// (and, from knife 3, the handle helpers) — they need a translation unit that
// sees the complete member types.
EngineContext::EngineContext() = default;
EngineContext::~EngineContext() = default;
