#ifndef CODESCOPE_UTIL_PATH_UTIL_H
#define CODESCOPE_UTIL_PATH_UTIL_H

#include <filesystem>
#include <string>
#include <vector>

namespace util
{

/// Absolute, symlink-resolved form of a path — the shape the store keeps.
///
/// `weakly_canonical` rather than `canonical`: it resolves the prefix that
/// exists and normalises the rest lexically, so ".", "./foo", "foo/../bar" and
/// absolute paths are all handled uniformly, including paths that do not exist
/// yet (an index target named on the command line, a project root that is
/// about to be created). Falls back to the input when resolution fails, so
/// callers that only need a best effort keep working.
///
/// This is the ONE implementation of the rule. It is what `createProject` uses
/// to register a project root (store_core.cpp), and therefore what every stored
/// `file_path` is built from; callers that build a path to look up must use it
/// too, or an exact / prefix match against those rows misses.
inline std::string resolvePath(const std::string &path)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path resolved = fs::weakly_canonical(fs::path(path), ec);
	if (ec)
		return path;
	// Native form on POSIX so the string is byte-identical to what was stored.
#ifdef _WIN32
	return resolved.string();
#else
	return resolved.native();
#endif
}

/// The form of a caller-supplied path to match against stored `file_path`
/// values.
///
/// Only an ABSOLUTE input is resolved. These arguments are documented as "the
/// absolute file path" but the SQL wraps them in `file_path LIKE '%'||?||'%'`,
/// so they double as a substring filter: "src/main.go" or "impact_analysis"
/// must stay exactly as given, because resolving either against the current
/// directory would turn a working filter into a wrong one. An absolute path,
/// however, is meant to name the file, and the stored row is usually resolved —
/// a caller reaching the project through a symlink matched nothing and read as
/// "no such symbol" (reproduced: indexing /tmp/pn/real and filtering on
/// /tmp/pn/link/src/main.go returned `{"results":[],"total":0}` for a symbol
/// that exists).
inline std::string lookupPath(const std::string &path)
{
	if (!std::filesystem::path(path).is_absolute())
		return path;
	return resolvePath(path);
}

/// The LIKE patterns a caller-supplied path filter must be matched with: the
/// value exactly as given, and its resolved form.
///
/// Two patterns rather than one, because the stored rows are not guaranteed to
/// be resolved either: `createProject` resolves the root it registers, but an
/// index driven through the engine API keeps whatever prefix the caller passed
/// (measured — a fixture indexed as `/tmp/...` holds `/tmp/...` file_path rows,
/// while the CLI-indexed ones hold `/private/tmp/...`). Resolving the filter
/// alone therefore trades a miss on one database for a miss on the other;
/// accepting both is additive and matches whichever form that database holds.
/// When the two forms are identical, so are the patterns, and the second test
/// is a harmless repeat of the first.
inline std::vector<std::string> lookupPatterns(const std::string &path)
{
	const std::string resolved = lookupPath(path);
	return { "%" + path + "%", "%" + resolved + "%" };
}

} // namespace util

#endif // CODESCOPE_UTIL_PATH_UTIL_H
