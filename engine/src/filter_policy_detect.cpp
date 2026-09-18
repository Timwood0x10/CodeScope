// filter_policy_detect.cpp — language detection and path/ignore matching.
//
// Split out of filter_policy.cpp (see plan/rules/code_rules.md 1000-line
// rule). Everything here answers "is this path or filename something we
// refuse to index": language detection (extension, then shebang probe),
// component-wise path matching, .gitignore-style rule matching, and the
// gitignore loader. The static skip/accept sets built by the constructor
// stay in filter_policy.cpp; this file only reads them.

#include "filter_policy.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

const char *FilterPolicy::detectLanguage(const char *file_path) const
{
	// Guard against null / empty path — strrchr below would dereference it
	if (!file_path || !*file_path)
		return nullptr;

	const char *ext = strrchr(file_path, '.');
	if (!ext) {
		// No extension: probe the shebang line to identify scripts
		// (e.g. "#!/usr/bin/env python3", "#!/usr/bin/env node").
		std::ifstream f(file_path);
		if (!f.is_open())
			return nullptr;
		std::string first_line;
		std::getline(f, first_line);
		if (first_line.size() >= 2 && first_line[0] == '#' &&
		    first_line[1] == '!') {
			// Match the interpreter token after the last '/' on the
			// shebang line so "/usr/bin/env python3" → "python3".
			auto sp = first_line.rfind('/');
			std::string interp = (sp != std::string::npos) ?
						     first_line.substr(sp + 1) :
						     first_line.substr(2);
			// Strip trailing whitespace/args
			auto ws = interp.find_first_of(" \t\r\n");
			if (ws != std::string::npos)
				interp.erase(ws);
			// Lowercase for case-insensitive comparison
			for (auto &c : interp)
				c = static_cast<char>(std::tolower(c));
			if (interp.rfind("python", 0) == 0)
				return "python";
			if (interp == "node" || interp == "nodejs" ||
			    interp == "deno")
				return "javascript";
			if (interp == "bash" || interp == "sh" ||
			    interp == "zsh" || interp == "ksh" ||
			    interp == "fish")
				return "bash";
			if (interp == "ruby" || interp == "rb")
				return "ruby";
			if (interp == "perl" || interp == "perl5")
				return "perl";
			if (interp == "php")
				return "php";
			if (interp == "lua")
				return "lua";
			if (interp == "rscript" || interp == "r")
				return "r";
			if (interp == "awk" || interp == "gawk" ||
			    interp == "mawk")
				return "awk";
		}
		return nullptr;
	}

	// Lowercase the extension into a small buffer so that .PY / .Rs /
	// .TSX on case-insensitive filesystems (Windows, default macOS)
	// are recognized identically to their canonical lowercase forms.
	std::string lext;
	lext.reserve(16);
	for (const char *p = ext; *p; ++p)
		lext.push_back(static_cast<char>(std::tolower(*p)));

	// Skip minified/bundled JS — generated code, expensive & low-value.
	// Use both '/' and '\\' so basename extraction works on Windows too.
	const char *slash_f = strrchr(file_path, '/');
	const char *slash_b = strrchr(file_path, '\\');
	const char *slash = (slash_b > slash_f) ? slash_b : slash_f;
	const char *fname = slash ? slash + 1 : file_path;
	size_t fname_len = strlen(fname);
	// Lowercased basename tail checks for robustness on all platforms.
	if (fname_len > 7) {
		char tail[8] = { 0 };
		for (size_t i = 0; i < 7; i++)
			tail[i] = static_cast<char>(
				std::tolower(fname[fname_len - 7 + i]));
		if (strcmp(tail, ".min.js") == 0)
			return nullptr;
	}
	if (fname_len > 10 && strstr(fname, ".bundle.js") != nullptr)
		return nullptr;
	if (strcmp(fname, "vendor.js") == 0)
		return nullptr;

	if (lext == ".py")
		return "python";
	if (lext == ".cpp" || lext == ".cc" || lext == ".cxx")
		return "cpp";
	if (lext == ".c")
		return "c";
	// C++ headers (.h/.hpp/.hxx/.hh) are parsed as C++ because the
	// tree-sitter C++ grammar is a strict superset of the C grammar.
	// Parsing .h as C would mis-handle templates, namespaces, classes,
	// and other C++ constructs commonly found in .h files.
	if (lext == ".h" || lext == ".hpp" || lext == ".hxx" || lext == ".hh")
		return "cpp";
	if (lext == ".rs")
		return "rust";
	if (lext == ".swift")
		return "swift";
	if (lext == ".js" || lext == ".mjs" || lext == ".cjs")
		return "javascript";
	if (lext == ".ts")
		return "typescript";
	if (lext == ".tsx")
		return "tsx";
	if (lext == ".go")
		return "go";
	if (lext == ".java")
		return "java";
	if (lext == ".kt" || lext == ".kts")
		return "kotlin";
	if (lext == ".rb")
		return "ruby";
	if (lext == ".scala")
		return "scala";

	return nullptr;
}

bool FilterPolicy::shouldSkipPath(const std::string &rel_path,
				  bool is_dir) const
{
	if (rel_path.empty())
		return false;

	// 1. Check every path component against active_skip_dirs_
	//    (this catches .venv, node_modules, etc. at any depth)
	std::istringstream ss(rel_path);
	std::string component;
	while (std::getline(ss, component, '/')) {
		if (shouldSkipDir(component))
			return true;
	}

	// 1b. Shallow skip dirs — source-bearing but "rarely the focus
	//     of analysis" dirs (test/, tests/, docs/, vendor/, bench/,
	//     samples/, ...). For non-Java projects these are already
	//     caught at ANY depth by the active_skip_dirs_ check above
	//     (step 1), so this block is a no-op for them. For Java
	//     projects, the shouldSkipDir() Java carve-out defers these
	//     names HERE so nested package components (e.g.
	//     src/main/java/org/springframework/samples/petclinic) are
	//     NOT clobbered — only the first 3 path components are checked
	//     against java_protected_skip_dirs_, catching:
	//       <root>/test/...                  (depth 1)
	//       <root>/src/test/java/...         (depth 2, Maven/Gradle)
	//       <root>/packages/<name>/tests/... (depth 3, Lerna monorepo)
	//     without touching deep package namespaces. See README.md
	//     "Why Java is the (only) exception".
	{
		// Extract up to 3 leading path components (depth <= 3).
		std::string head;
		size_t off = 0;
		for (int depth = 0; depth < 3; ++depth) {
			auto slash = rel_path.find('/', off);
			if (slash == std::string::npos) {
				head = rel_path;
				break;
			}
			off = slash + 1;
		}
		if (head.empty()) {
			// rel_path has at least 3 components — take first 3.
			head = rel_path.substr(0, off);
		}
		// Lowercase for case-insensitive comparison.
		for (auto &c : head)
			c = static_cast<char>(std::tolower(c));
		std::istringstream ss2(head);
		std::string comp;
		while (std::getline(ss2, comp, '/')) {
			if (comp.empty())
				continue;
			// Depth-gated for EVERY language: the
			// business-plausible dir names (public/, scripts/,
			// integration/, static/, ...). Skipping these at any
			// depth silently dropped real source files.
			if (top_only_skip_dirs_.find(comp) !=
			    top_only_skip_dirs_.end())
				return true;
			// Java carve-out for the names that remain any-depth
			// in active_skip_dirs_ (test/docs/samples/...). See
			// the comment in java_protected_skip_dirs_.
			if (lang_context_ == "java" &&
			    java_protected_skip_dirs_.find(comp) !=
				    java_protected_skip_dirs_.end())
				return true;
		}
	}

	// 2. Check against .gitignore rules (whole-path matching)
	if (!gitignore_rules_.empty() &&
	    gitignoreMatches(gitignore_rules_, rel_path, is_dir))
		return true;

	// 3. Check against .codescopeignore raw patterns
	//    Supports: directory/  (trailing slash = dir only)
	//              filename    (matches any component)
	//              /path/name  (anchored from root)
	for (const auto &pat : ignore_patterns_) {
		bool dir_only = (!pat.empty() && pat.back() == '/');
		bool anchored = (!pat.empty() && pat[0] == '/');
		// For dir_only patterns applied to file entries:
		// still check if any PATH COMPONENT matches the dir name
		if (dir_only && pat.size() <= 1)
			continue;

		// Normalize pattern: strip trailing /
		std::string normalized =
			dir_only ? pat.substr(0, pat.size() - 1) : pat;

		// Anchored: match from start
		if (anchored) {
			if (rel_path == normalized.substr(1) ||
			    (rel_path.size() > normalized.size() - 1 &&
			     rel_path.compare(0, normalized.size() - 1,
					      normalized.substr(1)) == 0 &&
			     rel_path[normalized.size() - 1] == '/'))
				return true;
			continue;
		}

		// Unanchored: check every path component
		std::istringstream ss2(rel_path);
		std::string comp;
		while (std::getline(ss2, comp, '/')) {
			if (comp == normalized)
				return true;
		}
	}

	return false;
}

bool FilterPolicy::shouldSkipEntry(const std::string &rel_path,
				   bool is_dir) const
{
	if (rel_path.empty())
		return false;

	// Normalize Windows backslashes to '/' so the rest of the pipeline
	// (shouldSkipPath splits on '/', detectLanguage uses strrchr('/'))
	// works uniformly across platforms.
	std::string normalized = rel_path;
	for (auto &c : normalized)
		if (c == '\\')
			c = '/';

	// Extract the basename once — used by every per-entry check below.
	// Use string_view into normalized to avoid copying (downstream calls
	// that need std::string construct it from this view).
	auto slash = normalized.rfind('/');
	size_t base_off = (slash == std::string::npos) ? 0 : slash + 1;
	std::string_view base(normalized.data() + base_off,
			      normalized.size() - base_off);

	// ── Shared checks (both dirs & files) ──
	// 1. Path-component skip_dirs (any depth) + gitignore + .codescopeignore.
	//    This alone catches node_modules/, .venv/, .git/ at any nesting.
	if (shouldSkipPath(normalized, is_dir))
		return true;

	if (is_dir) {
		// 2a. Bundle directory suffixes (.app, .framework, .xcodeproj ...)
		//     so we never recurse into binary payloads / IDE project bundles.
		if (shouldSkipDirSuffix(std::string(base)))
			return true;
		// Dir-only checks done; fall through to the shared exclude-pattern
		// check below so CODESCOPE_EXCLUDE_PATHS applies to directories too.
	} else {
		// ── File-only checks ──
		// 2b. Exact filename + filename-prefix skip (.env, .env.local, ...)
		if (shouldSkipFile(std::string(base)))
			return true;
		// 2c. Suffix skip — case-insensitive (.EXE == .exe). Catches
		//     binaries, archives, media, secrets, lock files, generated
		//     artifacts.
		auto dot = base.rfind('.');
		if (dot != std::string_view::npos) {
			if (shouldSkipSuffix(std::string(base.substr(dot))))
				return true;
		}

		// 2d. STRICT mode: whitelist gate — only files that
		//     detectLanguage() recognizes as source code pass through.
		//     Catches config/docs/data files that slipped past the
		//     blacklist (.toml, .yaml, .json, .md).
		if (mode_ == STRICT) {
			if (detectLanguage(normalized.c_str()) == nullptr)
				return true;
		}
	}

	// 3. User-specified exclude patterns (CODESCOPE_EXCLUDE_PATHS env var).
	//    Glob-matched against the full relative path so patterns like
	//    "test/*" or "vendor/**" skip both the directory and its contents.
	//    Applied LAST so it acts as a user override on top of all built-in
	//    filters. See loadExcludeEnv() for the env var format.
	for (const auto &pat : exclude_patterns_) {
		if (globMatch(pat, normalized))
			return true;
	}

	return false;
}

bool FilterPolicy::loadIgnoreFile(const std::string &project_root)
{
	std::string path = project_root + "/.codescopeignore";
	std::ifstream f(path);
	if (!f.is_open())
		return false;

	std::string line;
	ignore_patterns_.clear();
	while (std::getline(f, line)) {
		// Trim
		while (!line.empty() &&
		       (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		ignore_patterns_.push_back(line);
	}
	return !ignore_patterns_.empty();
}
