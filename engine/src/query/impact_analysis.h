#ifndef IMPACT_ANALYSIS_H
#define IMPACT_ANALYSIS_H

#include <cstdint>
#include <string>
#include <vector>

#include "../store/store.h"

namespace query {

/**
 * Performs change impact analysis on code graphs.
 *
 * Given a list of modified file paths, finds all impacted functions
 * (directly modified + their callers/callees) by traversing the
 * persisted graph (nodes + edges) in the store.
 *
 * All public functions return valid JSON. Errors are reported in the
 * JSON payload rather than via exceptions or return codes.
 */

/**
 * Analyze the impact of changes to the given files.
 *
 * @param project_id  The project to analyze.
 * @param store       Pointer to an initialized GraphStore.
 * @param modified_files_json  JSON array of modified file paths, e.g.
 *                             ["/path/to/file1.py", "/path/to/file2.cpp"].
 * @return JSON object:
 *   {
 *     "modified": [ { node fields } ],        // functions directly in changed
 * files "callers":  [ { node fields, caller_of } ], // functions that call
 * modified ones "callees":  [ { node fields, callee_of } ], // functions called
 * by modified ones "total_impacted": N
 *   }
 */
std::string analyzeChangeImpact(uint64_t project_id, store::GraphStore *store,
                                const char *modified_files_json);

} // namespace query

#endif // IMPACT_ANALYSIS_H
