// engine_filter_ffi.cpp — the filter decisions the server needs, answered by
// the SAME FilterPolicy the indexer uses.
//
// Why they are exported instead of reimplemented in Rust: the server's module
// discovery (`server/src/discover.rs`) needs to know which directories and
// files the worker will actually index, so the scheduler's allocation and the
// quarantine file list match reality. It used to keep hand-written copies of
// the C++ skip lists and of the source-extension list, and those copies drifted
// twice in one review:
//
//   * a `.gitignore`d TOP-LEVEL directory (this repository's `build-*`) was
//     still reported as a module, because the copy reads no ignore file at all
//     — the worker then indexed zero files for it;
//   * `.zig` was counted as a source extension although the engine has no Zig
//     support and none is planned: no extension mapping in detectLanguage(),
//     no translator. Measured on OmniScope, 362 `.zig` files were counted as
//     source while the index contained zero zig entities — the count was
//     advertising a capability the engine does not have. (Ruby, which the same
//     list contains, IS recognized by the engine and stays countable: the fix
//     is to follow the engine, not to shorten a list.)
//
// Both questions now have exactly one implementation — this file delegates to
// FilterPolicy — so a skip rule or a supported extension is added once.

#include "filter_policy.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
// One policy per project root: constructing a FilterPolicy fills a dozen skip
// sets from ~200 literals, and loading its ignore files touches the disk, so
// the per-path queries below must not repeat that work. The map is keyed by
// root because .gitignore/.codescopeignore are per project; the queries
// themselves are read-only. Guarded by a mutex: the server calls these from
// its walker.
std::mutex g_policy_mutex;
std::unordered_map<std::string, std::unique_ptr<FilterPolicy>> g_policies;

FilterPolicy &policyFor(const std::string &project_root)
{
	std::lock_guard<std::mutex> lock(g_policy_mutex);
	auto it = g_policies.find(project_root);
	if (it == g_policies.end()) {
		auto policy = std::make_unique<FilterPolicy>();
		if (!project_root.empty()) {
			// Same order as engine_index_project: the scan directory's own
			// rules first (they take precedence), project root's second.
			policy->loadIgnoreFile(project_root);
			policy->loadGitignore(project_root);
		}
		policy->loadExcludeEnv();
		it = g_policies.emplace(project_root, std::move(policy)).first;
	}
	return *it->second;
}
} // namespace

extern "C" int engine_path_is_skipped(const char *project_root,
				      const char *rel_path, int is_dir)
{
	// A filter query must never take the caller down: on any failure the
	// answer is "do not skip", which is the behaviour the callers had before
	// this function existed.
	if (!rel_path || !*rel_path)
		return 0;
	try {
		return policyFor(project_root ? project_root : "")
				       .shouldSkipEntry(rel_path, is_dir != 0) ?
			       1 :
			       0;
	} catch (...) {
		return 0;
	}
}

extern "C" int engine_is_indexable_source(const char *file_path)
{
	// "Can the engine parse this file?" — asked by extension (and shebang for
	// extensionless scripts), NOT "is it in someone's allow-list". This is
	// what keeps the server's file count from advertising languages the
	// engine cannot read.
	if (!file_path || !*file_path)
		return 0;
	try {
		return policyFor("").detectLanguage(file_path) != nullptr ? 1 :
									    0;
	} catch (...) {
		return 0;
	}
}
