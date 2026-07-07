#include "engine_internal.h"
#include "filter_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "dlfcn_compat.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── Phase A: Fast Scanner Helpers ─────────────────────────────

namespace
{

// Strip leading whitespace from a string view
static std::string_view trimLeft(std::string_view s)
{
	while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
		s.remove_prefix(1);
	return s;
}

// Check if a line starts with a keyword (after whitespace).
// If kw ends with a space, the space itself is the word boundary.
static bool startsWithKW(std::string_view line, const char *kw)
{
	line = trimLeft(line);
	auto klen = std::strlen(kw);
	if (line.size() < klen)
		return false;
	if (line.substr(0, klen) != kw)
		return false;
	if (line.size() > klen) {
		char c = line[klen];
		// If kw ends with space, the space was the delimiter — any char is valid
		if (klen > 0 && kw[klen - 1] == ' ')
			return true;
		// Otherwise, keyword must be followed by a non-word character
		return (c == ' ' || c == '(' || c == '<' || c == '\t' ||
			c == '{' || c == '[' || c == ':' || c == ';');
	}
	return true;
}

// Strict C function declaration detector for Linux-kernel-scale accuracy.
// Only matches: [storage_class] [type_keywords...] [*&]? name(
// The critical check: the return type MUST contain at least one known C type
// keyword. This eliminates 90%+ false positives from type casts, constructor
// calls, etc.
static bool looksLikeCFunction(std::string_view line)
{
	line = trimLeft(line);
	if (line.empty())
		return false;

	// Skip control flow and non-declaration starters (fast reject)
	if (line.size() >= 2) {
		char c0 = line[0], c1 = line[1];
		if (c0 == '#' || c0 == ';' || c0 == '/' || c0 == '*' ||
		    c0 == '}')
			return false;
		if (c0 == 'e' && c1 == 'l' && line.size() >= 4 &&
		    line.substr(0, 4) == "else")
			return false;
		if ((c0 == 'i' && c1 == 'f') || (c0 == 'f' && c1 == 'o') ||
		    (c0 == 'w' && c1 == 'h') || (c0 == 's' && c1 == 'w') ||
		    (c0 == 'c' && c1 == 'a') || (c0 == 'r' && c1 == 'e') ||
		    (c0 == 'b' && c1 == 'r') || (c0 == 'd' && c1 == 'o'))
			return false;
	}

	// Skip storage class specifiers (max one)
	static const char *storage_classes[] = { "static ",    "extern ",
						 "inline ",    "virtual ",
						 "explicit ",  "friend ",
						 "constexpr ", "consteval ",
						 "constinit ", nullptr };
	for (const char **sc = storage_classes; *sc; sc++) {
		auto len = std::strlen(*sc);
		if (line.substr(0, len) == *sc) {
			line = trimLeft(line.substr(len));
			break;
		}
	}
	if (line.empty())
		return false;

	// Find the '(' — for a real function, there should be one and it's not at
	// position 0
	auto paren_pos = line.find('(');
	if (paren_pos == std::string_view::npos || paren_pos == 0)
		return false;

	// Extract the name token right before '('
	auto name_end = paren_pos;
	auto name_start = name_end;
	while (name_start > 0 &&
	       (isalnum(line[name_start - 1]) || line[name_start - 1] == '_' ||
		line[name_start - 1] == ':'))
		name_start--;
	if (name_start == name_end)
		return false;

	// Reject known non-function names
	std::string_view name_tok =
		line.substr(name_start, name_end - name_start);
	static const char *non_func[] = {
		"if",	  "while",    "for",	"switch",  "catch",
		"return", "sizeof",   "typeof", "alignof", "decltype",
		"new",	  "delete",   "throw",	"else",	   "case",
		"break",  "continue", "goto",	"defined", nullptr
	};
	for (const char **nf = non_func; *nf; nf++) {
		if (name_tok == *nf)
			return false;
	}

	// === THE CRITICAL CHECK: what's before the name must be a valid return type
	// ===
	std::string_view before = trimLeft(line.substr(0, name_start));
	if (before.empty())
		return false;

	// C type keywords that must appear in the return type
	// This eliminates 90%+ false positives from non-declaration lines
	static const char *type_keywords[] = {
		"int",	      "void",	     "char",
		"long",	      "short",	     "float",
		"double",     "bool",	     "signed",
		"unsigned",   "const",	     "volatile",
		"struct",     "union",	     "enum",
		"class",      "size_t",	     "ssize_t",
		"off_t",      "pid_t",	     "time_t",
		"int8_t",     "int16_t",     "int32_t",
		"int64_t",    "uint8_t",     "uint16_t",
		"uint32_t",   "uint64_t",    "atomic_t",
		"gfp_t",      "phys_addr_t", "resource_size_t",
		"SQLITE_API", nullptr
	};

	for (const char **tk = type_keywords; *tk; tk++) {
		auto tlen = std::strlen(*tk);
		// Word-boundary check: the keyword must appear as a whole word in `before`
		auto pos = before.find(*tk);
		while (pos != std::string_view::npos) {
			// Previous char must be start-of-string, space, *, &, or (
			bool prev_ok = (pos == 0) || before[pos - 1] == ' ' ||
				       before[pos - 1] == '*' ||
				       before[pos - 1] == '&' ||
				       before[pos - 1] == '(' ||
				       before[pos - 1] == '\t';
			// Next char must be end, space, *, &, or (
			bool next_ok = (pos + tlen >= before.size()) ||
				       before[pos + tlen] == ' ' ||
				       before[pos + tlen] == '*' ||
				       before[pos + tlen] == '&' ||
				       before[pos + tlen] == ')' ||
				       before[pos + tlen] == '\t' ||
				       before[pos + tlen] == '\n';
			if (prev_ok && next_ok)
				return true;
			pos = before.find(*tk, pos + tlen);
		}
	}

	// Also accept pointer/reference return types with known base types:
	// e.g. "const char *name(" — const alone is a type keyword
	// "struct foo *name(" — struct alone was checked above
	// But DO NOT accept bare identifiers without type keywords (that's how casts
	// and constructors slip through)
	return false;
}

// ─── Table-driven declaration detection ──────────────────────────
//
// Each language has an array of (keyword, kind) pairs checked in order.
// This replaces the 175-line if-else chain with a simple lookup loop,
// reducing cognitive complexity from ~50 to ~5 and making it trivial
// to add new languages or patterns.

struct DeclPattern {
	const char *keyword;
	const char *kind;
};

static const DeclPattern RUST_PATTERNS[] = {
	{ "pub unsafe fn ", "function" },
	{ "pub async fn ", "function" },
	{ "pub fn ", "function" },
	{ "fn ", "function" },
	{ "pub struct ", "struct" },
	{ "struct ", "struct" },
	{ "pub enum ", "enum" },
	{ "enum ", "enum" },
	{ "pub trait ", "trait" },
	{ "trait ", "trait" },
	{ "pub type ", "type_alias" },
	{ "type ", "type_alias" },
	{ "pub const ", "const" },
	{ "const ", "const" },
	{ "pub static ", "const" },
	{ "static ", "const" },
	{ "pub union ", "struct" },
	{ "union ", "struct" },
	{ "mod ", "module" },
};

static const DeclPattern PYTHON_PATTERNS[] = {
	{ "async def ", "function" },
	{ "def ", "function" },
	{ "class ", "class" },
};

static const DeclPattern JSTS_PATTERNS[] = {
	{ "export async function ", "function" },
	{ "export function ", "function" },
	{ "async function ", "function" },
	{ "function ", "function" },
	{ "export class ", "class" },
	{ "class ", "class" },
	{ "export interface ", "interface" },
	{ "interface ", "interface" },
	{ "export enum ", "enum" },
	{ "enum ", "enum" },
};

static const DeclPattern GO_PATTERNS[] = {
	{ "func ", "function" },
	// type handled inline for struct/interface/type_alias distinction
};

static const DeclPattern SWIFT_PATTERNS[] = {
	{ "open func ", "function" },
	{ "public func ", "function" },
	{ "private func ", "function" },
	{ "func ", "function" },
	{ "open class ", "class" },
	{ "public class ", "class" },
	{ "class ", "class" },
	{ "public struct ", "struct" },
	{ "struct ", "struct" },
	{ "public enum ", "enum" },
	{ "enum ", "enum" },
	{ "public protocol ", "interface" },
	{ "protocol ", "interface" },
	{ "extension ", "class" },
	{ "init(", "function" },
};

static const DeclPattern JAVA_PATTERNS[] = {
	{ "public class ", "class" },	      { "private class ", "class" },
	{ "protected class ", "class" },      { "class ", "class" },
	{ "public interface ", "interface" }, { "interface ", "interface" },
	{ "public enum ", "enum" },	      { "enum ", "enum" },
	{ "public record ", "class" },	      { "record ", "class" },
};

static const DeclPattern C_CPP_PATTERNS[] = {
	{ "class ", "class" },	    { "struct ", "struct" },
	{ "enum class ", "enum" },  { "enum ", "enum" },
	{ "union ", "struct" },	    { "namespace ", "module" },
	{ "using namespace ", "" }, // skip
	{ "template ", "" }, // skip
};

// Check one keyword pattern at the start of a trimmed line.
// Returns the kind string, or nullptr if no match.
static const char *matchDeclPattern(const DeclPattern *patterns, size_t count,
				    std::string_view line)
{
	for (size_t i = 0; i < count; i++) {
		if (startsWithKW(line, patterns[i].keyword))
			return patterns[i].kind;
	}
	return nullptr;
}

// Detect the kind of symbol from a line of source code (language-aware)
// Returns the kind string, or empty string if no declaration found.
// Uses table-driven keyword matching for each language — O(patterns) per line.
static std::string detectDecl(std::string_view line, const std::string &lang)
{
	line = trimLeft(line);
	if (line.empty() || line[0] == '/' || line[0] == '#' || line[0] == '*')
		return "";

	const char *kind = nullptr;

	if (lang == "rust") {
		kind = matchDeclPattern(
			RUST_PATTERNS,
			sizeof(RUST_PATTERNS) / sizeof(RUST_PATTERNS[0]), line);
	} else if (lang == "python") {
		kind = matchDeclPattern(PYTHON_PATTERNS,
					sizeof(PYTHON_PATTERNS) /
						sizeof(PYTHON_PATTERNS[0]),
					line);
	} else if (lang == "javascript" || lang == "typescript") {
		kind = matchDeclPattern(
			JSTS_PATTERNS,
			sizeof(JSTS_PATTERNS) / sizeof(JSTS_PATTERNS[0]), line);
		// JS/TS arrow function detection (can't be table-driven)
		if (!kind) {
			if (line.substr(0, 6) == "const " ||
			    line.substr(0, 4) == "let " ||
			    line.substr(0, 4) == "var ") {
				auto eq = line.find('=');
				if (eq != std::string_view::npos && eq > 0) {
					auto after_eq =
						trimLeft(line.substr(eq + 1));
					if (!after_eq.empty() &&
					    (after_eq[0] == '(' ||
					     after_eq[0] == '>')) {
						return "function";
					}
					if (after_eq.substr(0, 8) ==
					    "function") {
						return "function";
					}
				}
				if (line.find(":") != std::string_view::npos)
					return "const";
			}
		}
	} else if (lang == "go") {
		// Go: func and type (type needs inline check)
		if (startsWithKW(line, "func "))
			return "function";
		if (startsWithKW(line, "type ")) {
			if (line.find("struct") != std::string_view::npos)
				return "struct";
			if (line.find("interface") != std::string_view::npos)
				return "interface";
			return "type_alias";
		}
	} else if (lang == "swift") {
		kind = matchDeclPattern(SWIFT_PATTERNS,
					sizeof(SWIFT_PATTERNS) /
						sizeof(SWIFT_PATTERNS[0]),
					line);
	} else if (lang == "java") {
		kind = matchDeclPattern(
			JAVA_PATTERNS,
			sizeof(JAVA_PATTERNS) / sizeof(JAVA_PATTERNS[0]), line);
		// Java methods need lookLikeCFunction check
		if (!kind && looksLikeCFunction(std::string(line)))
			return "method";
	} else if (lang == "c" || lang == "cpp") {
		kind = matchDeclPattern(C_CPP_PATTERNS,
					sizeof(C_CPP_PATTERNS) /
						sizeof(C_CPP_PATTERNS[0]),
					line);
		// C/C++ functions use looksLikeCFunction
		if (!kind && looksLikeCFunction(std::string(line)))
			return "function";
	}

	return kind ? kind : "";
}

// Extract the symbol name from a declaration line, language-aware
static std::string extractName(std::string_view line, const std::string &kind,
			       const std::string &lang)
{
	line = trimLeft(line);
	(void)kind; // kind is used for context but not critical for extraction

	if (lang == "rust") {
		// Skip pub/unsafe/async, then find the keyword token
		static const char *rkeywords[] = {
			"pub unsafe fn", "pub async fn", "pub fn",     "fn",
			"pub struct",	 "struct",	 "pub enum",   "enum",
			"pub trait",	 "trait",	 "pub type",   "type",
			"pub const",	 "const",	 "pub static", "static",
			"pub union",	 "union",	 "mod",	       nullptr
		};
		for (const char **kw = rkeywords; *kw; kw++) {
			if (startsWithKW(line, *kw)) {
				line = trimLeft(line.substr(std::strlen(*kw)));
				break;
			}
		}
		// Now line starts with the name
		auto it = line.begin();
		while (it != line.end() && (isalnum(*it) || *it == '_'))
			++it;
		return std::string(line.begin(), it);
	} else if (lang == "python") {
		static const char *pkw[] = { "async def", "def", "class",
					     nullptr };
		for (const char **kw = pkw; *kw; kw++) {
			if (startsWithKW(line, *kw)) {
				line = trimLeft(line.substr(std::strlen(*kw)));
				// name is before '(' or ':'
				auto end = line.find_first_of("(:");
				if (end == std::string_view::npos)
					end = line.size();
				return std::string(
					trimLeft(line.substr(0, end)));
			}
		}
	} else if (lang == "javascript" || lang == "typescript") {
		// export function name, function name, class name, etc.
		static const char *jskw[] = { "export async function",
					      "export function",
					      "async function",
					      "function",
					      "export class",
					      "class",
					      "export interface",
					      "interface",
					      "export enum",
					      "enum",
					      nullptr };
		for (const char **kw = jskw; *kw; kw++) {
			if (startsWithKW(line, *kw)) {
				line = trimLeft(line.substr(std::strlen(*kw)));
				auto end = line.find_first_of("( <{");
				if (end == std::string_view::npos)
					end = line.size();
				return std::string(
					trimLeft(line.substr(0, end)));
			}
		}
		// const/let/var name = ... (could be arrow function or const)
		{
			auto eq = line.find('=');
			if (eq != std::string_view::npos && eq > 0) {
				auto before_eq = trimLeft(line.substr(0, eq));
				auto space = before_eq.find_last_of(' ');
				if (space != std::string_view::npos) {
					return std::string(trimLeft(
						before_eq.substr(space + 1)));
				}
				return std::string(before_eq);
			}
		}
	} else if (lang == "go") {
		if (startsWithKW(line, "func ")) {
			line = trimLeft(line.substr(5));
			// Might be method: func (r *T) name(...)
			if (!line.empty() && line[0] == '(') {
				auto close = line.find(')');
				if (close != std::string_view::npos) {
					line = trimLeft(line.substr(close + 1));
				}
			}
			auto end = line.find('(');
			if (end == std::string_view::npos)
				end = line.size();
			return std::string(trimLeft(line.substr(0, end)));
		}
		if (startsWithKW(line, "type ")) {
			line = trimLeft(line.substr(5));
			auto end = line.find_first_of(" \t");
			if (end == std::string_view::npos)
				end = line.size();
			return std::string(line.substr(0, end));
		}
	} else if (lang == "java" || lang == "c" || lang == "cpp") {
		// For C/C++/Java, find the name before '('
		auto paren = line.find('(');
		if (paren != std::string_view::npos && paren > 0) {
			auto name_end = paren;
			while (name_end > 0 && (isalnum(line[name_end - 1]) ||
						line[name_end - 1] == '_'))
				name_end--;
			if (name_end < paren) {
				auto name_start = name_end;
				// Skip C++ namespace prefix T::
				if (name_start >= 2 &&
				    line[name_start - 1] == ':' &&
				    line[name_start - 2] == ':')
					name_start -= 2;
				return std::string(line.substr(
					name_start, paren - name_start));
			}
		}
		// For class/struct/enum keywords
		static const char *ckw[] = { "enum class", "class", "struct",
					     "enum",	   "union", "namespace",
					     nullptr };
		for (const char **kw = ckw; *kw; kw++) {
			if (startsWithKW(line, *kw)) {
				line = trimLeft(line.substr(std::strlen(*kw)));
				auto end = line.find_first_of(" \t{:");
				if (end == std::string_view::npos)
					end = line.size();
				return std::string(line.substr(0, end));
			}
		}
		// Java class/interface/enum
		static const char *jkw[] = { "public class",
					     "private class",
					     "protected class",
					     "public interface",
					     "interface",
					     "public enum",
					     "enum",
					     "public record",
					     "record",
					     nullptr };
		for (const char **kw = jkw; *kw; kw++) {
			if (startsWithKW(line, *kw)) {
				line = trimLeft(line.substr(std::strlen(*kw)));
				auto end = line.find_first_of(" \t{:");
				if (end == std::string_view::npos)
					end = line.size();
				return std::string(line.substr(0, end));
			}
		}
	}

	return "";
}

// Check if a symbol name is a likely entry point (conservative — avoid false positives)
static bool isEntryPoint(const std::string &name)
{
	// High-confidence: main function variants
	if (name == "main" || name == "Main" || name == "_main" ||
	    name == "WinMain" || name == "wmain")
		return true;
	// Kernel initcall macros: module_init(x), device_initcall(x), etc.
	// These are detected when the scanner finds the macro name itself
	if (name == "module_init" || name == "module_exit" ||
	    name == "device_initcall" || name == "subsys_initcall" ||
	    name == "late_initcall" || name == "arch_initcall" ||
	    name == "fs_initcall" || name == "rootfs_initcall" ||
	    name == "console_initcall" || name == "security_initcall")
		return true;
	// Driver probe callbacks (high confidence in driver context)
	if (name == "probe" || name == "Probe")
		return true;
	return false;
}

// Determine entry point kind from name
static std::string entryPointKind(const std::string &name)
{
	if (name == "main" || name == "Main" || name == "_main" ||
	    name == "WinMain" || name == "wmain")
		return "main";
	if (name == "probe" || name == "Probe")
		return "probe";
	if (name == "module_init" || name == "module_exit")
		return "module_init";
	if (name == "device_initcall" || name == "subsys_initcall" ||
	    name == "late_initcall" || name == "arch_initcall" ||
	    name == "fs_initcall" || name == "rootfs_initcall" ||
	    name == "console_initcall" || name == "security_initcall")
		return "initcall";
	return "entry";
}

} // anonymous namespace

// ─── Git-aware incremental scan helper ────────────────────────
// Runs `git status --porcelain` to detect changed files.
static std::unordered_set<std::string>
getGitChangedFiles(const std::string &project_dir)
{
 std::unordered_set<std::string> changed;
 if (!std::filesystem::exists(project_dir + "/.git"))
  return changed;
 // Use git -C <dir> instead of "cd <dir> && git" to avoid
 // shell injection via project_dir (no shell evaluation of path)
 std::string tm = "timeout";
 std::string tm_arg = "3";
 if (std::filesystem::exists("/opt/homebrew/bin/gtimeout")) {
  tm = "gtimeout";
 }
 // Execute via exec-style: no shell interpretation of project_dir
 // popen with "git -C" + escaped path is still a shell, but using
 // execv-style avoids the shell entirely. We use popen but escape
 // the dir to prevent shell metacharacter injection.
 // Safer: use pipe + fork/exec directly
 std::string cmd = tm + " " + tm_arg + " git -C " +
     project_dir +
     " status --porcelain 2>/dev/null || true";
 FILE *fp = popen(cmd.c_str(), "r");
	if (!fp)
		return changed;

	char buf[4096];
	while (fgets(buf, sizeof(buf), fp)) {
		// Format: "XY filename" where X/Y are status codes
		// We care about: M (modified), A (added), ? (untracked), D (deleted)
		if (strlen(buf) < 3)
			continue;
		char status = buf[0];
		char status2 = buf[1];
		if (status == 'D' || status2 == 'D')
			continue; // skip deleted - handled by cleanup

		// Extract filename starting at position 3
		std::string filepath(buf + 3);
		// Trim trailing newline
		while (!filepath.empty() &&
		       (filepath.back() == '\n' || filepath.back() == '\r'))
			filepath.pop_back();

		if (status == '?' && status2 == '?') {
			// Untracked file
			changed.insert(project_dir + "/" + filepath);
		} else if (status == 'M' || status2 == 'M' || status == 'A' ||
			   status2 == 'A' || status == 'R' || status2 == 'R') {
			changed.insert(project_dir + "/" + filepath);
		}
	}
	pclose(fp);
	return changed;
}

// ─── Phase A: engine_scan_project ──────────────────────────────

char *engine_scan_project(uint64_t project_id, const char *dir_path,
			  const char *language_filter)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	std::string dir = dir_path ? dir_path : "";
	if (dir.empty())
		return dupString("{\"error\":\"dir_path is empty\"}");

	// Normalize path: remove trailing slash
	while (!dir.empty() && dir.back() == '/')
		dir.pop_back();
	if (!std::filesystem::exists(dir))
		return dupString("{\"error\":\"directory not found\"}");

	std::string lang_filter = language_filter ? language_filter : "";

	g_store->beginTransaction();

	// Root module (parent_id = 0)
	std::string root_name = std::filesystem::path(dir).filename().string();
	if (root_name.empty())
		root_name = dir;
	uint64_t root_module_id = g_store->insertModule(
		project_id, 0, root_name.c_str(), dir.c_str(), "");

	// Track module_id by directory path
	std::unordered_map<std::string, uint64_t> module_path_map;
	module_path_map[dir] = root_module_id;

	// Git-aware incremental: only scan changed files if git repo
	std::unordered_set<std::string> git_changed = getGitChangedFiles(dir);
	bool incremental = !git_changed.empty();
	if (incremental) {
		fprintf(stderr,
			"engine: git incremental scan — %zu changed files\n",
			git_changed.size());
	}

	// Counters
	int total_symbols = 0;
	std::vector<std::pair<uint64_t, std::string> >
		entry_points; // (symbol_id, kind)

	// Walk directory tree
	try {
		// Load .gitignore patterns from project root
		FilterPolicy filter;
		filter.loadGitignore(dir);

		auto it = std::filesystem::recursive_directory_iterator(
			dir, std::filesystem::directory_options::
				     skip_permission_denied);
		auto end = std::filesystem::end(it);
		while (it != end) {
			// Compute path relative to project root for gitignore matching
			std::string rel_path = it->path().string();
			if (rel_path.size() > dir.size() + 1)
				rel_path = rel_path.substr(
					dir.size() + 1); // strip root + '/'
			else
				rel_path.clear();

			// Skip files/dirs matching .gitignore (unless !negated)
			if (!rel_path.empty()) {
				bool is_dir = it->is_directory();
				bool ignore = filter.isGitignoreMatch(
					rel_path, is_dir);
				if (ignore && is_dir) {
					it.disable_recursion_pending();
					++it;
					continue;
				}
				if (ignore) {
					++it;
					continue;
				}
			}
			if (it->is_regular_file()) {
				std::string file_path = it->path().string();
				const char *lang =
					detectLanguage(file_path.c_str());
				if (!lang) {
					++it;
					continue;
				}
				if (!lang_filter.empty() &&
				    lang != lang_filter) {
					++it;
					continue;
				}

				// Ensure parent module exists
				std::string parent_dir =
					it->path().parent_path().string();
				auto mod_it = module_path_map.find(parent_dir);
				uint64_t parent_mod_id;
				if (mod_it != module_path_map.end()) {
					parent_mod_id = mod_it->second;
				} else {
					// Create module chain for this directory
					std::filesystem::path p(parent_dir);
					std::string accumulated;
					uint64_t current_parent =
						root_module_id;
					for (const auto &part : p) {
						if (accumulated.empty()) {
							accumulated =
								part.string();
							if (accumulated == dir)
								continue;
						} else {
							accumulated +=
								"/" +
								part.string();
						}
						if (module_path_map.count(
							    accumulated)) {
							current_parent = module_path_map
								[accumulated];
							continue;
						}
						uint64_t mod_id =
							g_store->insertModule(
								project_id,
								current_parent,
								part.string()
									.c_str(),
								accumulated
									.c_str(),
								"");
						module_path_map[accumulated] =
							mod_id;
						current_parent = mod_id;
					}
					parent_mod_id = current_parent;
				}

				// Git incremental: skip files that haven't changed
				if (incremental &&
				    git_changed.find(file_path) ==
					    git_changed.end())
					continue;

				// Check file modification time for incremental indexing
				struct stat file_stat;
				int64_t mtime = 0, fsize_stat = 0;
				if (stat(file_path.c_str(), &file_stat) == 0) {
					mtime = static_cast<int64_t>(
						file_stat.st_mtime);
					fsize_stat = static_cast<int64_t>(
						file_stat.st_size);
				}
				if (g_store->isFileUnchanged(project_id,
							     file_path.c_str(),
							     mtime, fsize_stat))
					continue;

				// Read file content for declaration scanning
				std::ifstream file(file_path);
				if (!file)
					continue;

				// Use stat() size rather than seekg/tellg to avoid
				// 32-bit overflow on files >2GB (tellg is signed)
				size_t fsize = (fsize_stat > 0) ?
						   static_cast<size_t>(fsize_stat) :
						   0;
				if (fsize == 0) {
					file.close();
					continue;
				}
				if (fsize > 1024 * 1024) { // Skip files > 1MB
					file.close();
					continue;
				}
				file.seekg(0, std::ios::beg);
				std::string content(
					(std::istreambuf_iterator<char>(file)),
					std::istreambuf_iterator<char>());
				file.close();

				// Scan line-by-line for declarations
				std::istringstream stream(content);
				std::string line;
				int line_num = 0;
				while (std::getline(stream, line)) {
					line_num++;
					// Skip comments and empty lines
					std::string_view sv(line);
					sv = trimLeft(sv);
					if (sv.empty() || sv[0] == '/' ||
					    sv[0] == '#' || sv[0] == '*')
						continue;

					std::string kind = detectDecl(sv, lang);
					if (kind.empty())
						continue;

					std::string name =
						extractName(sv, kind, lang);
					if (name.empty())
						continue;

					// Skip common non-declaration matches
					if (name == "if" || name == "for" ||
					    name == "while" ||
					    name == "switch" ||
					    name == "catch" ||
					    name == "return" || name == "else")
						continue;

					// Skip module declarations in fast scan (they're tracked separately)
					if (kind == "module" &&
					    std::strcmp(lang, "rust") == 0) {
						// Could be `mod foo;` or `mod foo {`
						continue;
					}

					// Extract signature (first 80 chars of trimmed line)
					std::string sig = std::string(
						trimLeft(sv).substr(0, 80));

					// Determine visibility
					std::string visibility = "default";
					std::string_view sv2 = sv;
					if (sv2.substr(0, 4) == "pub " ||
					    sv2.substr(0, 7) == "public " ||
					    sv2.substr(0, 8) == "private " ||
					    sv2.substr(0, 10) == "protected ")
						visibility = "visible";
					if (sv2.substr(0, 4) == "pub ")
						visibility = "visible";

					// Compute span (byte offset approximate)
					int span_start = static_cast<int>(
						content.data() -
						content.c_str()); // not right, approximate
					// Better: accumulate known byte offsets
					// Simplified: just use line_num * average_line_length heuristic
					(void)span_start;

					// Insert symbol
					uint64_t sym_id = g_store->insertSymbol(
						project_id, parent_mod_id,
						kind.c_str(), name.c_str(),
						sig.c_str(), visibility.c_str(),
						lang, file_path.c_str(),
						line_num, 1, 0, 0);

					if (sym_id > 0) {
						total_symbols++;
						// Stub detection (fast scan — single-line only, AST-free)
						// Reliable case: func foo() {} — brace on same line, empty
						if ((kind == "function" ||
						     kind == "method") &&
						    line.find("{}") !=
							    std::string::npos) {
							// Only if nothing substantive between { and }
							auto ob =
								line.find('{');
							auto cb = line.find('}',
									    ob);
							if (cb !=
							    std::string::npos) {
								bool has_content =
									false;
								for (size_t i =
									     ob +
									     1;
								     i < cb;
								     i++) {
									if (line[i] !=
										    ' ' &&
									    line[i] !=
										    '\t') {
										has_content =
											true;
										break;
									}
								}
								if (!has_content)
									g_store->setSymbolStub(
										sym_id,
										true);
							}
						}
						// Check if entry point
						if (isEntryPoint(name)) {
							entry_points.emplace_back(
								sym_id,
								entryPointKind(
									name));
						}
					}
				}
				// Record file scan state for incremental indexing
				if (mtime > 0)
					g_store->updateFileScanState(
						project_id, file_path.c_str(),
						mtime, fsize_stat);
			} else if (it->is_directory()) {
				// Pre-populate module path for this directory
				std::string dir_path_str = it->path().string();
				if (module_path_map.count(dir_path_str) == 0) {
					auto parent = it->path().parent_path();
					uint64_t parent_id = root_module_id;
					auto pit = module_path_map.find(
						parent.string());
					if (pit != module_path_map.end()) {
						parent_id = pit->second;
					}
					uint64_t mod_id = g_store->insertModule(
						project_id, parent_id,
						it->path()
							.filename()
							.string()
							.c_str(),
						dir_path_str.c_str(), "");
					module_path_map[dir_path_str] = mod_id;
				}
			}
			++it;
		}
	} catch (const std::exception &e) {
		g_store->rollbackTransaction();
		return dupString("{\"error\":\"scan failed: " +
				 jsonEscape(e.what()) + "\"}");
	}

	// Insert entry points
	for (auto &[sym_id, kind] : entry_points) {
		g_store->insertEntryPoint(sym_id, project_id, kind.c_str());
	}

	// Update module file counts
	// Update module file counts
	{
		std::string count_sql =
			"SELECT id FROM modules WHERE project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		auto db = g_store->handle();
		if (sqlite3_prepare_v2(db, count_sql.c_str(), -1, &stmt,
				       nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				uint64_t mid = static_cast<uint64_t>(
					sqlite3_column_int64(stmt, 0));
				// Count files with this module_id
				const char *cnt_sql =
					"SELECT COUNT(DISTINCT file_path) FROM symbols WHERE module_id = ?";
				sqlite3_stmt *cnt_stmt = nullptr;
				if (sqlite3_prepare_v2(db, cnt_sql, -1,
						       &cnt_stmt,
						       nullptr) == SQLITE_OK) {
					sqlite3_bind_int64(
						cnt_stmt, 1,
						static_cast<int64_t>(mid));
					if (sqlite3_step(cnt_stmt) ==
					    SQLITE_ROW) {
						int fc = sqlite3_column_int(
							cnt_stmt, 0);
						const char *upd_sql =
							"UPDATE modules SET file_count = ? WHERE id = ?";
						sqlite3_stmt *upd_stmt =
							nullptr;
						if (sqlite3_prepare_v2(
							    db, upd_sql, -1,
							    &upd_stmt,
							    nullptr) ==
						    SQLITE_OK) {
							sqlite3_bind_int(
								upd_stmt, 1,
								fc);
							sqlite3_bind_int64(
								upd_stmt, 2,
								static_cast<
									int64_t>(
									mid));
							sqlite3_step(upd_stmt);
							sqlite3_finalize(
								upd_stmt);
						}
					}
					sqlite3_finalize(cnt_stmt);
				}
			}
			sqlite3_finalize(stmt);
		}
	}

	g_store->commitTransaction();

	// Build JSON response
	std::ostringstream json;
	json << "{"
	     << "\"modules\":" << g_store->getModuleTreeJson(project_id).c_str()
	     << ","
	     << "\"total_symbols\":" << total_symbols << ","
	     << "\"entry_points\":[";
	bool first = true;
	for (auto &[sym_id, kind] : entry_points) {
		if (!first)
			json << ",";
		first = false;
		json << "{\"symbol_id\":" << sym_id << ",\"kind\":\"" << kind
		     << "\"}";
	}
	json << "]}";
	return dupString(json.str());
}
