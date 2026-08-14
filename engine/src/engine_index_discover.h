#ifndef ENGINE_INDEX_DISCOVER_H
#define ENGINE_INDEX_DISCOVER_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "filter_policy.h"

namespace engine_index_discover
{

// One candidate source file collected during discovery. Mirrors the
// per-file job tuple used by both the streaming pipeline and the
// in-memory (membulk) path.
struct FileJob {
	std::string path;
	std::string lang;
	size_t size = 0;
};

// Walk `dir` and collect candidate source files, applying the same
// FilterPolicy rules as the scanner (skip dirs, gitignore,
// .codescopeignore, bundle suffixes, filename/suffix skips, language
// filter). Also ingests the project-root README as a knowledge
// document and runs the incremental scan-state gate.
//
// @param project_id  Project to index.
// @param dir         Absolute project root (trailing separators removed).
// @param filter      [in/out] FilterPolicy; mutated (stats counters, Java
//                    lang-context flip) during the walk.
// @param scan_state  "path|mtime|size" tuples for incremental skips.
// @param jobs        [out] Collected candidate files (unsorted).
// @param is_reindex  [out] True if any file was skipped as unchanged.
// @param err_json    [out] JSON error payload when returning -1.
// @return 0 on success (jobs populated), -1 on scan error (err_json set).
// @throws nothing — filesystem exceptions are caught internally.
int collectFileJobs(uint64_t project_id, const std::string &dir,
		    FilterPolicy &filter,
		    const std::unordered_set<std::string> &scan_state,
		    std::vector<FileJob> &jobs, bool &is_reindex,
		    std::string &err_json);

} // namespace engine_index_discover

#endif // ENGINE_INDEX_DISCOVER_H
