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
//
// "Existing" covers both tables that store a file path: `entity` for files that
// were indexed, and `parse_failures` for files that never were. The second is
// what keeps a persistently unparseable file from collecting one identity per
// spelling — see knownSpellingFor.

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

/// The candidate spellings to test for `path`, most specific first.
/// \param root  Project root as recorded by create_project ("" if unknown).
/// \param path  Path as the caller spells it.
/// \return `path` itself plus, when it lies under `root`, its path relative to
///         the root, its "./"-prefixed form and its "<rootbasename>/" form.
inline std::vector<std::string> spellingCandidates(const std::string &root,
						   const std::string &path)
{
	std::vector<std::string> candidates{ path };
	if (!root.empty() && root != "/" && !path.empty() && path[0] == '/') {
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
	return candidates;
}

/// Look `candidates` up in one stored-path column, or return "".
/// \param table  Internal table name — a literal from this header, never
///               caller input, so the concatenation cannot be injected into.
inline std::string
lookupStoredSpelling(store::GraphStore *store, uint64_t project_id,
		     const char *table,
		     const std::vector<std::string> &candidates)
{
	if (!store || !store->handle() || candidates.empty())
		return "";
	std::string sql = std::string("SELECT file_path FROM ") + table +
			  " WHERE project_id=? AND file_path IN (";
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

/// The spelling under which `path` is ALREADY stored for this project, or ""
/// when the project has not indexed it yet.
inline std::string existingSpellingFor(store::GraphStore *store,
				       uint64_t project_id,
				       const std::string &root,
				       const std::string &path)
{
	if (path.empty())
		return "";
	return lookupStoredSpelling(store, project_id, "entity",
				    spellingCandidates(root, path));
}

/// The spelling under which `path` is already KNOWN to the project — indexed,
/// or recorded as a parse failure — or "" when it is a new file.
///
/// The parse_failures half is not redundant: a file that never parses has no
/// entity row, and those are exactly the files that accumulate parse_failures
/// rows. Looking only at `entity` therefore left them free to acquire a second
/// identity, because the walk-based entry points spell a path as the caller
/// passed it while the single-file entry points are handed canonicalised paths
/// (on macOS `/tmp` is a symlink, so the same file arrived as both
/// "/tmp/p/a.py" and "/private/tmp/p/a.py"). One file then held two
/// parse_failures rows with independent fail_count values, so no retry policy
/// could reason about it, and `get_parse_failures` listed the file twice.
inline std::string knownSpellingFor(store::GraphStore *store,
				    uint64_t project_id,
				    const std::string &root,
				    const std::string &path)
{
	if (path.empty())
		return "";
	const std::vector<std::string> candidates =
		spellingCandidates(root, path);
	const std::string indexed =
		lookupStoredSpelling(store, project_id, "entity", candidates);
	if (!indexed.empty())
		return indexed;
	return lookupStoredSpelling(store, project_id, "parse_failures",
				    candidates);
}

/// The spelling to store `path` under: the project's existing one when it
/// already knows the file (indexed, or a recorded parse failure), otherwise
/// `path` as given.
inline std::string indexSpellingFor(store::GraphStore *store,
				    uint64_t project_id,
				    const std::string &path)
{
	if (path.empty())
		return path;
	const std::string stored = knownSpellingFor(
		store, project_id, projectRootPath(store, project_id), path);
	return stored.empty() ? path : stored;
}

#endif // ENGINE_INDEX_PATHS_H
