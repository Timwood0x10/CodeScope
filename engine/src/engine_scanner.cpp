#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h> // waitpid, WIFEXITED, WEXITSTATUS, WTERMSIG
#include <unistd.h> // pipe, fork, execvp, dup2, read, close
#endif

// ─── Phase A: Fast Scanner Helpers ─────────────────────────────

namespace
{

// Strip leading whitespace from a string view
static std::string_view trimLeft(std::string_view s)
{
	while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r'))
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
		"int",		 "void",	"char",
		"long",		 "short",	"float",
		"double",	 "bool",	"signed",
		"unsigned",	 "const",	"volatile",
		"struct",	 "union",	"enum",
		"class",	 "size_t",	"ssize_t",
		"off_t",	 "pid_t",	"time_t",
		"int8_t",	 "int16_t",	"int32_t",
		"int64_t",	 "uint8_t",	"uint16_t",
		"uint32_t",	 "uint64_t",	"atomic_t",
		"gfp_t",	 "phys_addr_t", "resource_size_t",
		"SQLITE_API",	 "EXTERN_C",	"APICALL",
		"__init",	 "__exit",	"__devinit",
		"__attribute__", "__declspec",	"__stdcall",
		"__cdecl",	 "inline",	"typedef",
		"typename",	 "mutable",	"explicit",
		"virtual",	 "override",	"noexcept",
		nullptr
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
// Runs `git status --porcelain -z` to detect changed files. Captures
// git's stdout via a pipe WITHOUT going through a shell, so project_dir
// is passed as a single argv element and shell metacharacters (; | $ `)
// in the path cannot break out into a separate command (command injection).
// Parses NUL-separated output to correctly handle paths containing spaces,
// quotes, and renames ("R  newpath\0oldpath\0").
static std::unordered_set<std::string>
getGitChangedFiles(const std::string &project_dir)
{
	std::unordered_set<std::string> changed;
	if (!std::filesystem::exists(project_dir + "/.git"))
		return changed;

	// Build argv. project_dir is passed verbatim as one argv element —
	// execvp never interprets it as a shell, so it is injection-safe.
	// Wrap in timeout/gtimeout when present (also exec'd, never shelled)
	// to prevent a wedged git from stalling the scan. If no timeout
	// binary is found, run git directly (graceful: no hang protection).
	std::string timeout_bin;
	if (std::filesystem::exists("/opt/homebrew/bin/gtimeout"))
		timeout_bin = "/opt/homebrew/bin/gtimeout";
	else if (std::filesystem::exists("/usr/local/bin/gtimeout"))
		timeout_bin = "/usr/local/bin/gtimeout";
	else if (std::filesystem::exists("/usr/bin/timeout"))
		timeout_bin = "/usr/bin/timeout";

	std::vector<std::string> args;
	if (!timeout_bin.empty()) {
		args.push_back(timeout_bin);
		args.push_back("3");
	}
	args.push_back("git");
	args.push_back("-C");
	args.push_back(project_dir);
	args.push_back("status");
	args.push_back("--porcelain");
	args.push_back("-z");

	// Capture child stdout.
	std::string output;
	char buf[4096];
#ifdef _WIN32
	// Windows: _popen routes through cmd.exe, so the project_dir MUST be
	// validated to reject shell metacharacters before we let cmd.exe see
	// it. POSIX uses fork+execvp (no shell) and needs no such guard.
	auto hasShellMeta = [](const std::string &s) {
		for (char c : s)
			if (c == ';' || c == '|' || c == '&' || c == '$' ||
			    c == '`' || c == '(' || c == ')' || c == '"' ||
			    c == '\n' || c == '\r' || c == '%' || c == '^' ||
			    c == '!')
				return true;
		return false;
	};
	if (hasShellMeta(project_dir)) {
		fprintf(stderr,
			"engine: rejected project_dir with shell "
			"metacharacters for git status [module=scanner, method=getGitChangedFiles]\n");
		return changed;
	}
	std::string cmd =
		"git -C \"" + project_dir + "\" status --porcelain -z";
	FILE *fp = _popen(cmd.c_str(), "r");
	if (!fp) {
		fprintf(stderr,
			"engine: _popen(git status) failed [module=scanner, method=getGitChangedFiles]\n");
		return changed;
	}
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		output.append(buf, n);
	int rc = _pclose(fp);
	if (rc != 0)
		fprintf(stderr,
			"engine: git status exited %d [module=scanner, method=getGitChangedFiles]\n",
			rc);
#else
	int fds[2];
	if (pipe(fds) != 0) {
		fprintf(stderr,
			"engine: pipe() failed for git status: %s [module=scanner, method=getGitChangedFiles]\n",
			strerror(errno));
		return changed;
	}
	std::vector<char *> argv;
	argv.reserve(args.size() + 1);
	for (auto &a : args)
		argv.push_back(&a[0]);
	argv.push_back(nullptr);

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr,
			"engine: fork() failed for git status: %s [module=scanner, method=getGitChangedFiles]\n",
			strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return changed;
	}
	if (pid == 0) {
		// Child: wire stdout → pipe, then exec (NO shell).
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		execvp(argv[0], argv.data());
		_exit(127); // exec failed — child exits, parent reads empty pipe
	}
	// Parent: read all of git's stdout.
	close(fds[1]);
	ssize_t rn;
	while ((rn = read(fds[0], buf, sizeof(buf))) != 0) {
		if (rn < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr,
				"engine: read() from git failed: %s [module=scanner, method=getGitChangedFiles]\n",
				strerror(errno));
			break;
		}
		output.append(buf, static_cast<size_t>(rn));
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) != 0)
			fprintf(stderr,
				"engine: git status exited %d [module=scanner, method=getGitChangedFiles]\n",
				WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr,
			"engine: git status killed by signal %d [module=scanner, method=getGitChangedFiles]\n",
			WTERMSIG(status));
	}
#endif

	// Parse `git status --porcelain -z` output. Each entry is:
	//   "XY path" terminated by NUL.  For renames/copies (X='R'/'C') the
	//   NEW path is first, followed by a NUL, then the OLD path, then NUL.
	//   -z disables quoting, so spaces/unicode in paths are literal.
	size_t i = 0;
	while (i + 3 <= output.size()) {
		char x = output[i];
		char y = output[i + 1];
		size_t path_start = i + 3; // skip "XY "
		size_t path_end = output.find('\0', path_start);
		if (path_end == std::string::npos)
			break;
		std::string filepath =
			output.substr(path_start, path_end - path_start);

		bool is_rename = (x == 'R' || x == 'C' || y == 'R' || y == 'C');
		if (is_rename) {
			// Skip the second (old-path) NUL-terminated field.
			size_t old_end = output.find('\0', path_end + 1);
			if (old_end == std::string::npos)
				break;
			i = old_end + 1;
		} else {
			i = path_end + 1;
		}

		if (x == 'D' || y == 'D')
			continue; // deleted — handled by cleanup, not re-scan

		if (x == '?' && y == '?') {
			changed.insert(project_dir + "/" + filepath);
		} else if (x == 'M' || y == 'M' || x == 'A' || y == 'A' ||
			   x == 'R' || y == 'R' || x == 'C' || y == 'C' ||
			   x == 'T' || y == 'T') {
			changed.insert(project_dir + "/" + filepath);
		}
	}
	return changed;
}

// ─── Phase A: engine_scan_project ──────────────────────────────
// Redirected to engine_index_project with fast mode.
// Regex scan eliminated — tree-sitter is fast enough (727ms for 150K lines).

char *engine_scan_project(uint64_t project_id, const char *dir_path,
			  const char *language_filter)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	(void)language_filter;
	return engine_index_project(project_id, dir_path, nullptr);
}
