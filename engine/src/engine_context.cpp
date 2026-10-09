#include "engine_context.h"
#include "parser/parser.h"
#include "query/query_engine.h"
#include "store/store.h"

// Completes the two out-of-line special members declared in engine_context.h.
// Instantiating the unique_ptr constructors/deleters here is exactly why the
// header can keep its member types forward-declared.
//
// This TU holds no state: after TD-1 knife 3 the instance is heap-allocated by
// engine_create() and released by engine_destroy() (engine_lifecycle.cpp), so
// there is neither a namespace-scope object nor a function-local static left.

CodescopeEngine::CodescopeEngine() = default;

/// Destructor. Members are destroyed in reverse declaration order
/// (parser → query → store), which is the order engine_destroy() establishes
/// explicitly: the query engine may issue SQLite work, so it must go before the
/// store closes.
CodescopeEngine::~CodescopeEngine() = default;
