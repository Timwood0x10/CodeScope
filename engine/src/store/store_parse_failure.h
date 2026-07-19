#ifndef STORE_PARSE_FAILURE_H
#define STORE_PARSE_FAILURE_H

#include <cstdint>
#include <string>
#include <vector>

namespace store
{

/// Failure reason codes used in parse_failures.fail_reason.
/// Stored as TEXT so the table remains readable via `sqlite3` CLI;
/// callers convert via failReasonToString() before writing.
enum class FailReason : int {
	ParseNullTree = 0, ///< tree-sitter returned a null tree
	ReadEmpty, ///< file read returned empty bytes
	StatFailed, ///< stat() failed (file vanished / perms)
	VisitorException, ///< visitor->visit() threw std::exception
	VisitorUnknownThrow, ///< visitor->visit() threw unknown exception
	LanguageMissing, ///< no TSLanguage registered for this lang
};

/// Convert FailReason enum to its string label for storage.
const char *failReasonToString(FailReason r);

/// Check whether a file is a known parse failure that has reached the
/// retry threshold. Returns true if the file should be SKIPPED (i.e.
/// fail_count >= retry_max). Returns false otherwise (including when
/// the file is not in the table, or fail_count < retry_max).
///
/// `retry_max` is read from `CODESCOPE_FAIL_RETRY_MAX` env var
/// (default 3) by the caller and passed in to avoid repeated env reads.
bool isKnownParseFailure(uint64_t project_id, const std::string &file_path,
			 int retry_max);

/// Record a parse failure for a file. If the row already exists,
/// increments fail_count and updates last_seen. Otherwise inserts a
/// new row with fail_count=1. Idempotent — safe to call concurrently
/// from multiple threads within the same process (SQLite serialises
/// via the writer-thread connection).
///
/// `lang` may be empty if the language was never identified.
/// `reason` is one of FailReason above (exception variants should
/// append the what() text after a colon, e.g. "exception: bad_alloc").
void recordParseFailure(uint64_t project_id, const std::string &file_path,
			const std::string &lang, const std::string &reason);

/// Reset (delete) all parse_failures rows for the given project_id.
/// Called by the `codescope reset-failures` CLI. Returns the number
/// of rows deleted, or -1 on error.
int resetParseFailures(uint64_t project_id);

/// Return a JSON array of all parse_failures rows for a project,
/// limited to `limit` rows (default 100). Each row is an object:
///   {"file_path":"...","language":"...","fail_reason":"...",
///    "fail_count":N,"first_seen":N,"last_seen":N}
/// Returns "[]" on error or empty table.
std::string getParseFailuresJson(uint64_t project_id, int limit);

/// Load all file_path values for the given project_id where
/// fail_count >= retry_max into the provided vector. Used at index
/// startup to pre-populate a skip set without per-file DB queries.
/// Returns true on success, false on DB error (caller logs).
bool loadKnownParseFailures(uint64_t project_id, int retry_max,
			    std::vector<std::string> &out_paths);

} // namespace store

#endif // STORE_PARSE_FAILURE_H
