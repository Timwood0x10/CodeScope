#include "filter_policy.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

FilterPolicy::FilterPolicy()
{
	// Normal mode skip dirs — VCS, IDE, build artifacts, dependency/env dirs.
	// Applied to EVERY path component so matches work at any depth
	// (e.g. src/node_modules, packages/foo/.venv are both caught).
	// NOTE: source-bearing dirs that are rarely the focus of analysis
	// (test/, docs/, examples/, vendor/, bench/) are included here
	// so NORMAL mode skips them. They would otherwise inflate file
	// counts by 3-5x (e.g. Bun: 3,251 → 9,935) and cause timeouts
	// on large projects.
	normal_skip_dirs_ = {
		// ── VCS & worktrees ──
		".git",
		".svn",
		".hg",
		".worktrees",
		".claude-worktrees",
		// ── IDE / editor state ──
		".vscode",
		".idea",
		".eclipse",
		".claude",
		"Antigravity",
		".clangd",
		".ccls-cache",
		".cache",
		".cpcache",
		".vs",
		// ── Language tooling caches ──
		".shadow-cljs",
		".metals",
		".bloop",
		".bsp",
		".mypy_cache",
		".pytest_cache",
		".ruff_cache",
		".nox",
		".tox",
		".eggs",
		".cargo",
		".stack-work",
		".dart_tool",
		"zig-cache",
		"zig-out",
		"elm-stuff",
		"_opam",
		".swiftpm",
		".tscache",
		".awcache",
		// ── JS/TS ecosystems ──
		".npm",
		".nyc_output",
		".pnpm-store",
		".yarn",
		".pnp",
		".next",
		".nuxt",
		".svelte-kit",
		".angular",
		".turbo",
		".parcel-cache",
		".docusaurus",
		".expo",
		".nx",
		"node_modules",
		"bower_components",
		"jspm_packages",
		".deno",
		".bun",
		// ── Python environments (non-standard projects often omit these
		//    from .gitignore, so we MUST hard-skip) ──
		".venv",
		"venv",
		"env",
		".env",
		"__pycache__",
		"site-packages",
		".virtualenv",
		".virtualenvs",
		// ── Build outputs ──
		"build",
		".build",
		"dist",
		".dist",
		"out",
		".out",
		"target",
		"Pods",
		"Carthage",
		"DerivedData",
		"bazel-bin",
		"bazel-out",
		"bazel-testlogs",
		".gradle",
		".buck",
		".buck-out",
		"obj",
		"Debug",
		"Release",
		"_build",
		".mvn",
		"cmake-build-debug",
		"cmake-build-release",
		// ── Coverage / test reports ──
		"coverage",
		"htmlcov",
		// ── IaC / serverless ──
		".terraform",
		".terragrunt-cache",
		".serverless",
		".vercel",
		".netlify",
		// ── Tool-internal & misc ──
		".codescope",
		".codegraph",
		".qdrant_code_embeddings",
		"temp",
		"tmp",
		".tmp",
		// ── Secrets (NEVER index) ──
		".ssh",
		// ── Generated / vendor / test (inflate file count 3-5x) ──
		"generated",
		"gen",
		"auto-generated",
		"fixtures",
		"testdata",
		"test_data",
		"__tests__",
		"__mocks__",
		"__snapshots__",
		"__fixtures__",
		"__test__",
		"test",
		"tests",
		"docs",
		"doc",
		"documentation",
		"examples",
		"example",
		"samples",
		"sample",
		"scripts",
		"tools",
		"hack",
		"migrations",
		"seeds",
		"e2e",
		"integration",
		"locale",
		"locales",
		"i18n",
		"l10n",
		"assets",
		"static",
		"public",
		"media",
		"external",
		"bin",
		"third_party",
		"thirdparty",
		"3rdparty",
		"vendor",
		"bench",
		"benchmark",
		"benchmarks",
	};

	// FAST mode skips even more — additional dirs that are sometimes
	// source-bearing but rarely the focus of a quick scan.
	fast_extra_skip_dirs_ = {};

	// Skip suffixes — non-source binaries, images, archives, etc.
	// Matched CASE-INSENSITIVELY so .EXE / .Dll on Windows are caught.
	// Covers: platform executables, object code, packages/installers,
	// debug symbols, media, fonts, archives, secrets, data files, logs.
	skip_suffixes_ = {
		// ── Compiled object code & libraries (all platforms) ──
		".o",
		".obj",
		".a",
		".lib",
		".so",
		".dll",
		".dylib",
		".lo",
		".la",
		".slo",
		// ── Native executables & installers ──
		".exe",
		".bin",
		".app",
		".msi",
		".scr",
		".cpl",
		".drv",
		".ocx",
		".efi",
		".com",
		".bat",
		".cmd",
		".ps1",
		// ── Packages / disk images ──
		".deb",
		".rpm",
		".dmg",
		".pkg",
		".snap",
		".flatpak",
		".appimage",
		".apk",
		".aab",
		".aar",
		".ipa",
		".xpi",
		".crx",
		// ── Kernel modules / drivers ──
		".ko",
		".kext",
		// ── Bytecode / VM artifacts ──
		".class",
		".wasm",
		".node",
		".pyc",
		".pyo",
		".pyd",
		".beam",
		".elc",
		".rlib",
		".cmo",
		".cma",
		".cmi",
		".cmx",
		".hi",
		".native",
		".run",
		// ── Debug symbols ──
		".pdb",
		".dbg",
		".dwarf",
		".dwo",
		".sym",
		// ── MSVC build intermediates ──
		".ilk",
		".idb",
		".exp",
		".tlog",
		".lastbuildstate",
		".unsuccessfulbuild",
		// ── Images ──
		".png",
		".jpg",
		".jpeg",
		".gif",
		".ico",
		".bmp",
		".tiff",
		".webp",
		".svg",
		".heic",
		".heif",
		".avif",
		".raw",
		// ── Fonts ──
		".woff",
		".woff2",
		".ttf",
		".eot",
		".otf",
		// ── Audio / video ──
		".mp3",
		".mp4",
		".avi",
		".mov",
		".wav",
		".flac",
		".ogg",
		".mkv",
		".webm",
		".m4a",
		".aac",
		".m4v",
		".wmv",
		// ── Documents ──
		".pdf",
		".doc",
		".docx",
		".xls",
		".xlsx",
		".ppt",
		".pptx",
		".odt",
		".ods",
		".odp",
		".epub",
		// ── Archives ──
		".zip",
		".tar",
		".gz",
		".tgz",
		".bz2",
		".xz",
		".rar",
		".7z",
		".lz",
		".lzma",
		".zst",
		".cab",
		".cpio",
		// ── JVM artifacts ──
		".jar",
		".war",
		".ear",
		// ── Source maps (generated, not source) ──
		".map",
		// ── Secrets / certs (NEVER index) ──
		".pem",
		".crt",
		".key",
		".p12",
		".pfx",
		".jks",
		".keystore",
		// ── Data files ──
		".pb",
		".avro",
		".parquet",
		".feather",
		".orc",
		".h5",
		".hdf5",
		".npy",
		".npz",
		".pkl",
		".pickle",
		// ── Databases ──
		".db",
		".sqlite",
		".sqlite3",
		".mdb",
		".accdb",
		// ── Logs / temp / backups ──
		".log",
		".tmp",
		".swp",
		".swo",
		".bak",
		".orig",
		".rej",
		// ── Coverage / profiling output ──
		".coverage",
		".prof",
		".profraw",
		".gcda",
		".gcno",
		".gcov",
		// ── Editor / IDE metadata ──
		".iml",
		// ── Lock files (generated, not source) ──
		".lock",
		// ── Tilde backup (vim/emacs) ──
		"~",
	};
	fast_extra_suffixes_ = {
		".min.js",
		".min.css",
	};

	// Directory suffixes — bundle / package / IDE project DIRECTORIES.
	// Matched case-insensitively against the directory's basename so
	// "Foo.app", "Foo.APP" and "GLFW.framework" are all skipped.
	skip_dir_suffixes_ = {
		".app",		".framework",	  ".bundle",
		".plugin",	".kext",	  ".xcodeproj",
		".xcworkspace", ".xcdatamodeld",  ".scnassets",
		".xcassets",	".playground",	  ".playgroundpackage",
		".docc",	".assetscatalog",
	};

	// Skip filenames — exact match (case-insensitive: entries are
	// lowercased at construction, and shouldSkipFile lowercases the
	// input before lookup).
	skip_filenames_ = {
		// ── Lock files (generated) ──
		"package-lock.json",
		"yarn.lock",
		"pnpm-lock.yaml",
		"Gemfile.lock",
		"Cargo.lock",
		"composer.lock",
		"go.sum",
		"poetry.lock",
		"Pipfile.lock",
		"gradle.lockfile",
		"gradle.properties",
		// ── OS metadata ──
		".DS_Store",
		"Thumbs.db",
		"desktop.ini",
		".lsyncd.cfg",
		// ── Secrets (NEVER index — .env* also via prefix below) ──
		".env",
		".env.local",
		".env.production",
		".env.development",
		".env.staging",
		".env.test",
		".env.example",
		".npmrc",
		".yarnrc",
		".pypirc",
		".netrc",
		".p12",
		// ── Misc generated / non-source ──
		".gitkeep",
		".gitattributes",
		".editorconfig",
		"yarn-error.log",
	};

	// Skip filename prefixes — files whose name STARTS WITH one of these.
	// All entries MUST be lowercase — shouldSkipFile lowercases the input
	// before comparison, so entries are matched case-insensitively.
	skip_filename_prefixes_ = {
		".env.", // .env.local, .env.production, .env.development.local ...
	};

	// Normalize all lookup sets to lowercase so the case-insensitive
	// lookups in shouldSkipDir/shouldSkipFile/shouldSkipSuffix (which
	// lowercase the query) actually match. Without this, mixed-case
	// entries like "Pods", "Cargo.lock", ".DS_Store" would NEVER match.
	auto lowercaseAll = [](std::unordered_set<std::string> &s) {
		std::unordered_set<std::string> tmp;
		tmp.reserve(s.size());
		for (const auto &e : s) {
			std::string lower = e;
			for (auto &c : lower)
				c = static_cast<char>(std::tolower(c));
			tmp.insert(std::move(lower));
		}
		s.swap(tmp);
	};
	lowercaseAll(normal_skip_dirs_);
	lowercaseAll(fast_extra_skip_dirs_);
	lowercaseAll(skip_suffixes_);
	lowercaseAll(fast_extra_suffixes_);
	lowercaseAll(skip_dir_suffixes_);
	lowercaseAll(skip_filenames_);
	lowercaseAll(skip_filename_prefixes_);

	buildActiveSets();
}

void FilterPolicy::buildActiveSets()
{
	active_skip_dirs_ = normal_skip_dirs_;
	if (mode_ == FAST) {
		active_skip_dirs_.insert(fast_extra_skip_dirs_.begin(),
					 fast_extra_skip_dirs_.end());
	}
}

void FilterPolicy::setLanguageFilter(const std::string &filter)
{
	if (filter.empty()) {
		has_lang_filter_ = false;
		return;
	}
	has_lang_filter_ = true;
	lang_filter_set_.clear();
	size_t start = 0, end;
	do {
		end = filter.find(',', start);
		auto lang = filter.substr(start, end - start);
		// Normalize to lowercase
		for (auto &c : lang)
			c = static_cast<char>(std::tolower(c));
		lang_filter_set_.insert(lang);
		start = end + 1;
	} while (end != std::string::npos);
}

bool FilterPolicy::isLanguageAccepted(const std::string &lang) const
{
	if (!has_lang_filter_)
		return true;
	return lang_filter_set_.find(lang) != lang_filter_set_.end();
}

bool FilterPolicy::shouldSkipDir(const std::string &dir_name) const
{
	// Case-insensitive: lowercase before lookup so Node_Modules / VENV / BIN
	// match on case-sensitive filesystems (Linux ext4).
	std::string lower = dir_name;
	for (auto &c : lower)
		c = static_cast<char>(std::tolower(c));
	if (active_skip_dirs_.find(lower) != active_skip_dirs_.end())
		return true;
	return false;
}

bool FilterPolicy::shouldSkipFile(const std::string &filename) const
{
	// Case-insensitive: lowercase before lookup so .ENV.LOCAL matches.
	std::string lower = filename;
	for (auto &c : lower)
		c = static_cast<char>(std::tolower(c));
	if (skip_filenames_.find(lower) != skip_filenames_.end())
		return true;
	// Prefix check — catches .env.local, .env.production, etc.
	for (const auto &pfx : skip_filename_prefixes_) {
		if (lower.size() >= pfx.size() &&
		    lower.compare(0, pfx.size(), pfx) == 0)
			return true;
	}
	// Vim/emacs backup files (main.cpp~, config.json~) — the trailing '~'
	// can't be matched by the '.'-extension path above, so check it here.
	if (!lower.empty() && lower.back() == '~')
		return true;
	return false;
}

bool FilterPolicy::shouldSkipSuffix(const std::string &ext) const
{
	// Case-insensitive: lowercase the extension before lookup so .EXE /
	// .Dll / .SO on Windows & case-insensitive macOS filesystems match.
	std::string lower = ext;
	for (auto &c : lower)
		c = static_cast<char>(std::tolower(c));
	if (skip_suffixes_.find(lower) != skip_suffixes_.end())
		return true;
	if (mode_ == FAST) {
		if (fast_extra_suffixes_.find(lower) !=
		    fast_extra_suffixes_.end())
			return true;
	}
	return false;
}

// Check whether a directory's basename ends with a known bundle suffix
// (case-insensitive). Used to skip .app/.framework/.xcodeproj dirs.
bool FilterPolicy::shouldSkipDirSuffix(const std::string &dir_name) const
{
	for (const auto &sfx : skip_dir_suffixes_) {
		if (dir_name.size() >= sfx.size()) {
			auto tail =
				dir_name.substr(dir_name.size() - sfx.size());
			for (auto &c : tail)
				c = static_cast<char>(std::tolower(c));
			if (tail == sfx)
				return true;
		}
	}
	return false;
}

bool FilterPolicy::isSourceFile(const std::string &path) const
{
	// Check suffix
	auto dot = path.rfind('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = path.substr(dot);
	if (shouldSkipSuffix(ext))
		return false;

	// Check filename
	auto slash = path.rfind('/');
	std::string fname =
		(slash == std::string::npos) ? path : path.substr(slash + 1);
	if (shouldSkipFile(fname))
		return false;

	return true;
}

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
	if (lext == ".c" || lext == ".h")
		return "c";
	if (lext == ".hpp" || lext == ".hxx")
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
		return false;
	}

	// ── File-only checks ──
	// 2b. Exact filename + filename-prefix skip (.env, .env.local, lock files)
	if (shouldSkipFile(std::string(base)))
		return true;
	// 2c. Suffix skip — case-insensitive (.EXE == .exe). Catches binaries,
	//     archives, media, secrets, lock files, generated artifacts.
	auto dot = base.rfind('.');
	if (dot != std::string_view::npos) {
		if (shouldSkipSuffix(std::string(base.substr(dot))))
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

bool FilterPolicy::loadGitignore(const std::string &project_root)
{
	std::string path = project_root + "/.gitignore";
	std::ifstream f(path);
	if (!f.is_open())
		return false;

	gitignore_rules_.clear();
	std::string line;
	while (std::getline(f, line)) {
		// Trim whitespace
		auto start = line.find_first_not_of(" \t\r");
		if (start == std::string::npos)
			continue;
		auto end = line.find_last_not_of(" \t\r");
		line = line.substr(start, end - start + 1);

		if (line.empty() || line[0] == '#')
			continue;

		GitignoreRule rule;
		// Negation
		if (line[0] == '!') {
			rule.negate = true;
			line = line.substr(1);
		}
		// Directory-only
		if (!line.empty() && line.back() == '/') {
			rule.dir_only = true;
			line.pop_back();
		}
		// Anchored
		if (!line.empty() && line[0] == '/') {
			rule.anchored = true;
			line = line.substr(1);
		}
		// Check for glob wildcards
		rule.has_star = (line.find('*') != std::string::npos);
		rule.pattern = line;
		if (!rule.pattern.empty())
			gitignore_rules_.push_back(std::move(rule));
	}
	return !gitignore_rules_.empty();
}

// ── Static gitignore matching helpers ────────────────────────

bool FilterPolicy::gitignoreMatches(const std::vector<GitignoreRule> &rules,
				    const std::string &rel_path, bool is_dir)
{
	bool ignored = false;
	for (const auto &r : rules) {
		// Directory-only rule doesn't apply to files
		if (r.dir_only && !is_dir)
			continue;

		bool match = false;
		if (r.has_star) {
			// Per gitignore spec: non-anchored pattern without '/'
			// matches only the filename (last path component)
			if (!r.anchored &&
			    r.pattern.find('/') == std::string::npos) {
				auto pos = rel_path.rfind('/');
				auto basename =
					(pos == std::string::npos) ?
						rel_path :
						rel_path.substr(pos + 1);
				match = globMatch(r.pattern, basename);
			} else {
				match = globMatch(r.pattern, rel_path);
			}
		} else {
			// Simple literal match — fast path
			if (r.anchored) {
				match = (rel_path == r.pattern);
			} else {
				// Literal match at path-component boundaries only,
				// so "foo" matches "foo", "a/foo", "a/foo/b" but
				// not "afoo" or "foobar". Iterate ALL occurrences
				// (not just the last via rfind) so a pattern like
				// "foo" matches even when "xfoo" appears later.
				size_t search_from = 0;
				while (true) {
					auto pos = rel_path.find(r.pattern,
								 search_from);
					if (pos == std::string::npos)
						break;
					auto after = pos + r.pattern.size();
					bool left_boundary =
						(pos == 0 ||
						 rel_path[pos - 1] == '/');
					bool right_boundary =
						(after == rel_path.size() ||
						 rel_path[after] == '/');
					if (left_boundary && right_boundary) {
						match = true;
						break;
					}
					search_from = pos + 1;
				}
			}
		}

		if (match) {
			ignored = !r.negate;
			// If this is a positive match and not negated, stop early
			if (!r.negate)
				break;
		}
	}
	return ignored;
}

bool FilterPolicy::globMatch(const std::string &pattern, const std::string &str)
{
	auto pi = pattern.begin(), si = str.begin();
	return globImpl(pattern, str, pi, si);
}

bool FilterPolicy::globImpl(const std::string &p, const std::string &s,
			    std::string::const_iterator pi,
			    std::string::const_iterator si)
{
	while (pi != p.end()) {
		if (*pi == '*') {
			// ** matches anything
			if (pi + 1 != p.end() && *(pi + 1) == '*') {
				pi += 2; // skip "**"
				// **/ or /** - match any depth
				if (pi != p.end() && *pi == '/')
					pi++;
				// Try matching rest of pattern at every position
				while (si != s.end()) {
					if (globImpl(p, s, pi, si))
						return true;
					++si;
				}
				return globImpl(p, s, pi, si);
			}
			// * matches anything except /
			while (si != s.end() && *si != '/') {
				if (globImpl(p, s, pi + 1, si))
					return true;
				++si;
			}
			return globImpl(p, s, pi + 1, si);
		}
		if (si == s.end())
			return false;
		if (*pi != *si && *pi != '?')
			return false;
		++pi;
		++si;
	}
	return (si == s.end());
}

void FilterPolicy::printStats() const
{
	std::cerr << "FilterPolicy: seen_dirs=" << stats_.seen_dirs
		  << " seen_files=" << stats_.seen_files
		  << " skipped_dirs=" << stats_.skipped_dirs
		  << " skipped_files=" << stats_.skipped_files
		  << " skipped_suffix=" << stats_.skipped_suffix
		  << " skipped_lang=" << stats_.skipped_lang
		  << " candidate_files=" << stats_.candidate_files << "\n";
}
