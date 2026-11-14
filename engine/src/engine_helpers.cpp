#include "engine_internal.h"

#include "store/store_identity.h"
#include "util/loc_counter.h"
#include "filter_policy.h"
#include "platform_win.h"
#include "util/json_writer.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// ─── Helpers ───────────────────────────────────────────────────

/**
 * Read a file into a string using std::ifstream (portable, RAII).
 *
 * @param path  File path to read.
 * @return      File contents as string, or empty string on error.
 */
std::string readFile(const char *path)
{
	if (!path || !*path)
		return "";

	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs)
		return "";

	std::streamsize size = ifs.tellg();
	if (size <= 0)
		return "";

	ifs.seekg(0, std::ios::beg);
	std::string result(static_cast<size_t>(size), '\0');
	if (!ifs.read(&result[0], size))
		return "";

	return result;
}

// v0.6 (perf): file read that avoids the ate-seek + tellg round-trip when
// the caller already knows the file size (e.g. it just stat()'d the file for
// the size limit check). Semantics are identical to readFile for a stable
// file: we open, read exactly `known_size` bytes and return them. If the
// file is smaller than known_size (truncated between stat and read), the
// read falls short and we return the bytes actually read; if it is larger,
// we return exactly the stat'd prefix — matching what the caller's size
// check already assumed. Returns "" on open failure or empty read.
std::string readFilePrealloc(const char *path, size_t known_size)
{
	if (!path || !*path || known_size == 0)
		return "";

	std::ifstream ifs(path, std::ios::binary);
	if (!ifs)
		return "";

	std::string result(known_size, '\0');
	std::streamsize n = static_cast<std::streamsize>(known_size);
	if (!ifs.read(&result[0], n))
		result.resize(static_cast<size_t>(ifs.gcount()));
	return result;
}

// ─── File identity + size, measured in one read ─────────────────
namespace
{

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void fnv1aUpdate(uint64_t &h, const char *data, std::size_t n)
{
	for (std::size_t i = 0; i < n; ++i) {
		h ^= static_cast<unsigned char>(data[i]);
		h *= kFnvPrime;
	}
}

/// 16 lowercase hex digits of a 64-bit FNV value.
std::string fnvHex(uint64_t h)
{
	static const char *hex = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 0; i < 16; ++i) {
		out[15 - i] = hex[h & 0xF];
		h >>= 4;
	}
	return out;
}

} // namespace

// M2: stable 64-bit FNV-1a hash of a file's contents, hex-encoded lowercase.
// Reads the file from disk (streamed, not into memory) and returns the hash,
// or "" if the file cannot be read. See declaration in engine_internal.h.
std::string fileContentHash(const char *path)
{
	if (!path || !*path)
		return "";
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs)
		return "";
	uint64_t h = kFnvOffsetBasis;
	char buf[65536];
	while (ifs) {
		ifs.read(buf, sizeof(buf));
		fnv1aUpdate(h, buf, static_cast<std::size_t>(ifs.gcount()));
	}
	return fnvHex(h);
}

FileMeasurement measureFileBytes(const char *path)
{
	FileMeasurement out;
	if (!path || !*path)
		return out;
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs)
		return out;
	std::stringstream ss;
	ss << ifs.rdbuf();
	const std::string source = ss.str();

	// Same bytes, same hash as fileContentHash(): the incremental scan compares
	// the two, so the constants and the fold order must stay identical.
	uint64_t h = kFnvOffsetBasis;
	fnv1aUpdate(h, source.data(), source.size());
	out.fnv = fnvHex(h);

	out.sha256 = store::Sha256Hex(source);
	const util::LocBreakdown loc = util::countLoc(source);
	out.loc = loc.loc;
	out.code_loc = loc.code_loc;
	out.comment_loc = loc.comment_loc;
	out.blank_loc = loc.blank_loc;
	return out;
}

// Escape a string for safe embedding in JSON (RFC 8259)
const char *detectLanguage(const char *file_path)
{
	// Delegate to the canonical FilterPolicy implementation so there is a
	// SINGLE source of truth for language detection (case-insensitive
	// extensions, shebang probing for extensionless scripts, and the full
	// language set including .kt/.rb/.scala/.mjs/.cjs). The static local
	// is initialized once (thread-safe per C++11 [stmt.dcl]); it carries
	// no mutable per-call state, so concurrent reads are safe.
	static const FilterPolicy kCanonicalFilter;
	return kCanonicalFilter.detectLanguage(file_path);
}

/**
 * Duplicate a string into a new heap-allocated C string.
 *
 * MEMORY OWNERSHIP:
 * - Allocates memory using malloc()
 * - Caller MUST free() the returned pointer when no longer needed
 * - Returns nullptr if allocation fails
 *
 * @param s  String to duplicate
 * @return   Newly allocated C string, or nullptr on allocation failure
 */
char *dupString(const std::string &s)
{
	char *buf = static_cast<char *>(malloc(s.size() + 1));
	if (buf) {
		memcpy(buf, s.data(), s.size());
		buf[s.size()] = '\0';
	} else {
		// OOM: return a valid empty-JSON pointer instead of nullptr,
		// so FFI callers (which unconditionally engine_free_string the
		// result) don't pass nullptr to free().
		buf = static_cast<char *>(malloc(3));
		if (buf) {
			buf[0] = '{';
			buf[1] = '}';
			buf[2] = '\0';
		}
	}
	return buf;
}
