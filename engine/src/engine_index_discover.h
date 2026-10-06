#ifndef ENGINE_INDEX_DISCOVER_H
#define ENGINE_INDEX_DISCOVER_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine_context.h"
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
int collectFileJobs(EngineContext *ctx, uint64_t project_id,
		    const std::string &dir, FilterPolicy &filter,
		    const std::unordered_set<std::string> &scan_state,
		    std::vector<FileJob> &jobs, bool &is_reindex,
		    std::string &err_json);

// Ingest one README file into the `document` table as a knowledge document
// (type 0), replacing any previous row for the same path.
//
// The knowledge layer (CapabilityPlugin, ContractPlugin) and the drift tools
// read language claims out of README content, so the row must exist for those
// tools to be able to answer at all. `.md` files are in the FilterPolicy's
// skip list, which is why ingestion happens before the skip check rather than
// through the source-code path.
//
// Idempotent on purpose: the drift tools concatenate every README row of a
// project, so a duplicate would silently double the weight of its claims.
//
// @param ctx         Engine instance; its store receives the row. Must not be
//                    null and must have an open store.
// @param project_id  Project the document belongs to.
// @param readme_path Absolute path of the README file.
// @return true when the row was (re)written; false when the file had no
// content or the store rejected it (logged here, with the store error).
bool ingestReadmeDocument(EngineContext *ctx, uint64_t project_id,
			  const std::string &readme_path);

// Ingest the project's root README, if it has one.
//
// Every index path must end with the same documentation state, but ingestion
// used to live only in the discovery walk and only for the directories it
// scanned: the parallel scheduler indexes module directories, so a project
// whose root holds no source files never had its root README ingested
// (six of ten real projects checked had zero documents while the file was
// present), and the file-list path (force-index, force_index_files) ingested
// none at all. Enhancing is the one step every path runs last, so the root
// README is ingested there, before the knowledge layer reads it.
//
// @param ctx         Engine instance; its store must be open.
// @param project_id  Project whose root README should be ingested.
// @return 1 when a README was ingested, 0 when the project has none,
//         -1 when the project row could not be read (logged).
int ingestProjectRootReadme(EngineContext *ctx, uint64_t project_id);

} // namespace engine_index_discover

#endif // ENGINE_INDEX_DISCOVER_H
