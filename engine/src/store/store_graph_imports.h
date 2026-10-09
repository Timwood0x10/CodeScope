// store_graph_imports.h — batch import writer used by buildGraph.
//
// Split out of store_graph.cpp (the Rust/C++ rule is 1000 lines per file).
// The import scan collects (path, alias, file) triples and flushes them in
// batches so that one multi-row INSERT replaces one per row.
#ifndef CODESCOPE_STORE_GRAPH_IMPORTS_H
#define CODESCOPE_STORE_GRAPH_IMPORTS_H

#include <cstdint>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace store
{

/// One collected import statement.
struct PathRec {
	std::string path;
	std::string alias;
	std::string file;
};

/// Flush one batch of import rows.
/// @return false when the batch could not be written, so the caller can
///         fail the surrounding buildGraph instead of silently dropping
///         imports (a dropped import means a lost edge).
bool flushImportBatch(sqlite3 *db, uint64_t project_id,
		      const std::vector<PathRec> &batch);

} // namespace store

#endif // CODESCOPE_STORE_GRAPH_IMPORTS_H
