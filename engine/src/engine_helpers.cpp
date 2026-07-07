#include "engine_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Helpers ───────────────────────────────────────────────────

/**
 * Read a file into a string using pread (safe, no mmap edge cases).
 * Uses a stack buffer for small files to avoid heap allocation.
 *
 * @param path  File path to read.
 * @return      File contents as string, or empty string on error.
 */
std::string readFile(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return "";

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size == 0) {
		close(fd);
		return "";
	}

	size_t size = static_cast<size_t>(st.st_size);
	std::string result(size, '\0');

	// Handle EINTR: retry on signal interruption
	size_t total_read = 0;
	while (total_read < size) {
		ssize_t bytes_read =
			read(fd, &result[total_read], size - total_read);
		if (bytes_read < 0) {
			if (errno == EINTR) {
				// Signal received, retry
				continue;
			}
			// Other error
			close(fd);
			return "";
		}
		if (bytes_read == 0) {
			// EOF reached early
			break;
		}
		total_read += static_cast<size_t>(bytes_read);
	}
	close(fd);

	if (total_read != size)
		return "";

	return result;
}

// Escape a string for safe embedding in JSON (escape ", \, \n, \r, \t)
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
			out += c;
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
	const char *ext = strrchr(file_path, '.');
	if (!ext)
		return nullptr;

	// Skip minified/bundled JS files — they are typically generated code
	// that is expensive to parse and provides little semantic value.
	const char *slash = strrchr(file_path, '/');
	const char *fname = slash ? slash + 1 : file_path;
	size_t fname_len = strlen(fname);
	if (fname_len > 7 && strcmp(fname + fname_len - 7, ".min.js") == 0)
		return nullptr;
	if (fname_len > 10 && strstr(fname, ".bundle.js") != nullptr)
		return nullptr;
	if (strcmp(fname, "vendor.js") == 0)
		return nullptr;

	if (strcmp(ext, ".py") == 0)
		return "python";
	if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
	    strcmp(ext, ".cxx") == 0)
		return "cpp";
	if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0)
		return "c";
	if (strcmp(ext, ".hpp") == 0 || strcmp(ext, ".hxx") == 0)
		return "cpp";
	if (strcmp(ext, ".rs") == 0)
		return "rust";
	if (strcmp(ext, ".swift") == 0)
		return "swift";
	if (strcmp(ext, ".js") == 0)
		return "javascript";
	if (strcmp(ext, ".ts") == 0)
		return "typescript";
	if (strcmp(ext, ".tsx") == 0)
		return "tsx";
	if (strcmp(ext, ".go") == 0)
		return "go";
	if (strcmp(ext, ".java") == 0)
		return "java";
	return nullptr;
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
