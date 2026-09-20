#ifndef ENGINE_INDEX_PATHS_H
#define ENGINE_INDEX_PATHS_H

// How a file's path is spelled in the index.
//
// One file must have one identity, but two things decide the spelling and they
// disagree:
//
//   * the walk-based entry points store `filesystem::path(root_argument) /
//     entry`, so the spelling depends on the argument the caller passed —
//     `/abs/proj/src/a.go` for an absolute root, `./src/a.go` for ".", and
//     `proj/src/a.go` for a relative `proj`;
//   * the single-file entry points (`index_file`, `force_index_files`, the
//     scheduler's `--file-list` retry) are handed ABSOLUTE paths by their
//     callers and used to store them verbatim.
//
// Same file, two spellings → the entities exist twice, a query by the indexed
// spelling misses the new rows, and one run can count the file twice. Since the
// projects table only records the canonicalised root, the walk's spelling
// cannot be recomputed from it, so the existing spelling is looked up and
// reused. The common spellings are enumerated explicitly (no suffix guessing,
// which could match a different file), and the ones that are not covered leave
// the file with the old behaviour rather than a wrong one.

#include "store/store.h"

#include <sqlite3.h>

#include <string>
#include <vector>

/// The project's root directory as recorded by create_project ("" if unknown).
inline std::string projectRootPath(store::GraphStore *store,
				   uint64_t project_id)
{
	if (!store || !store->handle())
		return "";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(),
			       "SELECT root_path FROM projects WHERE id=?", -1,
			       &stmt, nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	std::string root;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (p)
			root = p;
	}
	sqlite3_finalize(stmt);
	return root;
}

/// The spelling under which `path` is ALREADY stored for this project, or ""
/// when the project has not indexed it yet.
inline std::string existingSpellingFor(store::GraphStore *store,
				       uint64_t project_id,
				       const std::string &root,
				       const std::string &path)
{
	if (!store || !store->handle() || path.empty())
		return "";
	std::vector<std::string> candidates{ path };
	if (!root.empty() && root != "/" && path[0] == '/') {
		std::string prefix = root;
		if (prefix.back() != '/')
			prefix.push_back('/');
		if (path.compare(0, prefix.size(), prefix) == 0) {
			const std::string rel = path.substr(prefix.size());
			if (!rel.empty()) {
				candidates.push_back(rel);
				candidates.push_back("./" + rel);
				const size_t slash = root.find_last_of('/');
				const std::string base =
					slash == std::string::npos ?
						root :
						root.substr(slash + 1);
				if (!base.empty())
					candidates.push_back(base + "/" + rel);
			}
		}
	}
	std::string sql =
		"SELECT file_path FROM entity WHERE project_id=? AND file_path IN (";
	for (size_t i = 0; i < candidates.size(); i++)
		sql += (i > 0 ? ",?" : "?");
	sql += ") LIMIT 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK)
		return "";
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	for (size_t i = 0; i < candidates.size(); i++)
		sqlite3_bind_text(stmt, static_cast<int>(i + 2),
				  candidates[i].c_str(), -1, SQLITE_TRANSIENT);
	std::string found;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (p)
			found = p;
	}
	sqlite3_finalize(stmt);
	return found;
}

/// The spelling to store `path` under: the existing one when the file is
/// already indexed, otherwise `path` as given.
inline std::string indexSpellingFor(store::GraphStore *store,
				    uint64_t project_id,
				    const std::string &path)
{
	if (path.empty())
		return path;
	const std::string stored = existingSpellingFor(
		store, project_id, projectRootPath(store, project_id), path);
	return stored.empty() ? path : stored;
}

#endif // ENGINE_INDEX_PATHS_H
