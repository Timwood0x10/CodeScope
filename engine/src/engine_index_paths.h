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
//
// Enumerating spellings is not enough when the two sides differ by a SYMLINKED
// ANCESTOR (`/tmp/x` vs `/private/tmp/x`): no spelling of the canonical path
// yields the aliased one, because realpath only goes one way. That case is
// closed by a second pass that narrows on the path tail and accepts a stored
// row only when its canonical form equals the incoming path's — equality, not
// a guess, so two files can never be merged.

#include "store/store.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

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
			       nullptr) != SQLITE_OK) {
		// Reported, then treated as "not stored": the caller falls back to
		// the spelling it was given (code_rules.md: no silent errors).
		fprintf(stderr,
			"engine: spelling lookup prepare failed for %s "
			"[module=engine, method=lookupStoredSpelling]\n",
			table);
		return "";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	for (size_t i = 0; i < candidates.size(); i++)
		sqlite3_bind_text(stmt, static_cast<int>(i + 2),
				  candidates[i].c_str(), -1, SQLITE_TRANSIENT);
	std::string found;
	const int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (p)
			found = p;
	} else if (rc != SQLITE_DONE) {
		fprintf(stderr,
			"engine: spelling lookup step failed (%d) for %s "
			"[module=engine, method=lookupStoredSpelling]\n",
			rc, table);
	}
	sqlite3_finalize(stmt);
	return found;
}

/// How many stored-path candidates the alias fallback inspects per table
/// before giving up. Bounds the work when many rows share a path tail.
constexpr int kAliasCandidateLimit = 16;

/// Absolute normalised form of `path`, or "" when it cannot be compared.
///
/// Uses weakly_canonical: it resolves the symlinked part of the path without
/// requiring the file to exist, so a file that was deleted (or that a crashed
/// run left behind) still compares equal to its other spelling. Relative paths
/// return "" — the walk recorded them relative to a working directory this
/// function cannot know, so resolving them here would be a guess.
inline std::string canonicalForComparison(const std::string &path)
{
	if (path.empty() || !std::filesystem::path(path).is_absolute())
		return "";
	std::error_code ec;
	const std::string canon =
		std::filesystem::weakly_canonical(path, ec).string();
	return ec ? std::string() : canon;
}

/// Escape the LIKE metacharacters in `s` so it matches literally.
/// \param s  Text to match against `file_path`.
/// \return `s` with `%`, `_` and `\` backslash-escaped (use with ESCAPE '\').
inline std::string escapeLikePattern(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (const char c : s) {
		if (c == '%' || c == '_' || c == '\\')
			out.push_back('\\');
		out.push_back(c);
	}
	return out;
}

/// Canonical form of a possibly relative `path` resolved against `base_dir`.
/// \param base_dir  Directory the relative path is resolved against.
/// \param path      Path to normalise.
/// \return Absolute normalised form, or "" when it cannot be resolved.
inline std::string canonicalRelativeTo(const std::string &base_dir,
				       const std::string &path)
{
	if (base_dir.empty() || path.empty())
		return "";
	std::error_code ec;
	const std::string joined =
		(std::filesystem::path(base_dir) / path).string();
	const std::string canon =
		std::filesystem::weakly_canonical(joined, ec).string();
	return ec ? std::string() : canon;
}

/// Second pass over the stored spellings: those that cannot be enumerated.
/// Rows are narrowed by their path tail and accepted only when they are
/// provably the same file as the incoming path — never on resemblance.
///
/// * an ABSOLUTE row is accepted when its canonical form equals
///   `canonical_incoming`. This is the symlinked-ancestor case (macOS
///   `/tmp/x` is really `/private/tmp/x`): indexing through the alias stores
///   "/tmp/x/f.c" while the single-file entry points are handed
///   "/private/tmp/x/f.c", and no spelling of the latter enumerates the
///   former. Without it the file reached `entity` twice and `find_symbol`
///   answered with its symbol twice.
/// * a RELATIVE row is accepted when it is "<prefix>/<rel>" and `prefix`
///   resolves, from the current working directory, to the project root. That
///   is what the walk stored for a relative root argument (`./proj/f.c`,
///   `../proj/f.c`) when the indexer ran from this directory; when it did not,
///   the check fails and the row is left alone rather than guessed at.
///
/// \param rel                 Path relative to the project root (no leading /).
/// \param canonical_incoming  canonicalForComparison() of the incoming path.
/// \param canonical_root      canonicalForComparison() of the project root.
inline std::string
lookupStoredSpellingByCanonical(store::GraphStore *store, uint64_t project_id,
				const char *table, const std::string &rel,
				const std::string &canonical_incoming,
				const std::string &canonical_root)
{
	if (!store || !store->handle() || rel.empty() ||
	    canonical_incoming.empty())
		return "";
	std::string sql = std::string("SELECT file_path FROM ") + table +
			  " WHERE project_id=? AND file_path LIKE ? ESCAPE '\\'"
			  " LIMIT " +
			  std::to_string(kAliasCandidateLimit);
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql.c_str(), -1, &stmt,
			       nullptr) != SQLITE_OK) {
		// Not fatal: a failed probe leaves the file with the ordinary
		// behaviour (it is stored under the spelling it came in with), but
		// it must not pass unnoticed (code_rules.md: no silent errors).
		fprintf(stderr,
			"engine: alias spelling probe prepare failed for %s "
			"[module=engine, method=lookupStoredSpellingByCanonical]\n",
			table);
		return "";
	}
	const std::string pattern = "%/" + escapeLikePattern(rel);
	const std::string suffix = "/" + rel;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);

	// Resolved once; empty disables the relative branch (see the contract).
	std::error_code cwd_ec;
	const std::string cwd = std::filesystem::current_path(cwd_ec).string();
	const bool can_check_relative = !cwd_ec && !cwd.empty() &&
					!canonical_root.empty();

	std::string found;
	int rc = SQLITE_DONE;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (!p)
			continue;
		const std::string stored(p);
		// LIKE narrowed it, but the acceptance test below is exact: the row
		// must really end with "/<rel>".
		if (stored.size() <= suffix.size() ||
		    stored.compare(stored.size() - suffix.size(), suffix.size(),
				   suffix) != 0)
			continue;
		if (std::filesystem::path(stored).is_absolute()) {
			if (canonicalForComparison(stored) ==
			    canonical_incoming) {
				found = stored;
				break;
			}
			continue;
		}
		if (!can_check_relative)
			continue;
		const std::string prefix =
			stored.substr(0, stored.size() - suffix.size());
		if (prefix.empty())
			continue;
		if (canonicalRelativeTo(cwd, prefix) == canonical_root) {
			found = stored;
			break;
		}
	}
	if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
		fprintf(stderr,
			"engine: alias spelling probe step failed (%d) for %s "
			"[module=engine, method=lookupStoredSpellingByCanonical]\n",
			rc, table);
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
/// Two passes, each over `entity` first and `parse_failures` second:
///   1. the enumerated spellings (path, root-relative, "./"-prefixed,
///      "<rootbasename>/" — see spellingCandidates);
///   2. spellings that CANNOT be enumerated, accepted only on canonical
///      equality (a symlinked ancestor, see lookupStoredSpellingByCanonical).
///
/// The parse_failures half is not redundant: a file that never parses has no
/// entity row, and those are exactly the files that accumulate parse_failures
/// rows. Looking only at `entity` therefore left them free to acquire a second
/// identity, because the walk-based entry points spell a path as the caller
/// passed it while the single-file entry points are handed canonicalised paths
/// (on macOS `/tmp` is a symlink, so the same file arrived as both
/// "/tmp/p/a.py" and "/private/tmp/p/a.py"). One file then held two
/// parse_failures rows with independent fail_count values, so no retry policy
/// could reason about it, and `get_parse_failures` listed the file twice —
/// and, for files that DO parse, `entity` held the symbols twice.
inline std::string knownSpellingFor(store::GraphStore *store,
				    uint64_t project_id,
				    const std::string &root,
				    const std::string &path)
{
	if (path.empty())
		return "";
	const std::vector<std::string> candidates =
		spellingCandidates(root, path);
	std::string found =
		lookupStoredSpelling(store, project_id, "entity", candidates);
	if (!found.empty())
		return found;
	found = lookupStoredSpelling(store, project_id, "parse_failures",
				     candidates);
	if (!found.empty())
		return found;

	// Second pass: `path` under the recorded root gives the tail to match on.
	if (root.empty() || root == "/" ||
	    !std::filesystem::path(path).is_absolute())
		return "";
	std::string prefix = root;
	if (prefix.back() != '/')
		prefix.push_back('/');
	if (path.compare(0, prefix.size(), prefix) != 0)
		return "";
	const std::string rel = path.substr(prefix.size());
	if (rel.empty())
		return "";
	const std::string canon = canonicalForComparison(path);
	if (canon.empty())
		return "";
	const std::string canonical_root = canonicalForComparison(root);
	found = lookupStoredSpellingByCanonical(store, project_id, "entity",
						rel, canon, canonical_root);
	if (!found.empty())
		return found;
	return lookupStoredSpellingByCanonical(store, project_id,
					       "parse_failures", rel, canon,
					       canonical_root);
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
