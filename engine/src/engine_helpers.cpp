#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// Escape a string for safe embedding in JSON (RFC 8259)
std::string jsonEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 4);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			// Escape control characters (0x00-0x1f) as \uXXXX
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x",
					 static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
			break;
		}
	}
	return out;
}

std::string simpleHash(const std::string &s)
{
	// Simple djb2 hash for content fingerprint
	uint64_t hash = 5381;
	for (char c : s)
		hash = ((hash << 5) + hash) + static_cast<uint64_t>(c);
	return std::to_string(hash);
}

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
	}
	return buf;
}
