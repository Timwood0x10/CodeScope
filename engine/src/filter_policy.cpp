#include "filter_policy.h"
#include <cstring>
#include <fstream>
#include <iostream>

FilterPolicy::FilterPolicy()
{
	// Normal mode skip dirs — VCS, IDE, common build artifacts
	normal_skip_dirs_ = {
		".git",
		".svn",
		".hg",
		".worktrees",
		".vscode",
		".idea",
		".eclipse",
		".claude",
		".claude-worktrees",
		"Antigravity",
		".clangd",
		".ccls-cache",
		".cache",
		".cpcache",
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
		".npm",
		".nyc_output",
		".pnpm-store",
		".yarn",
		".next",
		".nuxt",
		".svelte-kit",
		".angular",
		".turbo",
		".parcel-cache",
		".docusaurus",
		".expo",
		".cargo",
		".stack-work",
		".dart_tool",
		"zig-cache",
		"zig-out",
		"elm-stuff",
		"_opam",
		".terraform",
		".serverless",
		".vercel",
		".netlify",
		".codescope",
		".codegraph",
		"node_modules",
		"target",
		"__pycache__",
		".venv",
		"venv",
		"build",
		".build",
		"dist",
		".dist",
		"out",
		".out",
		"coverage",
		"htmlcov",
		"site-packages",
		"Pods",
		"temp",
		"tmp",
		"bazel-bin",
		"bazel-out",
		"bazel-testlogs",
		"third_party",
		"thirdparty",
		"3rdparty",
		"vendor",
		".qdrant_code_embeddings",
		".tmp",
	};

	// FAST mode skips even more — docs, examples, tests, generated, etc.
	fast_extra_skip_dirs_ = {
		"generated",	"gen",		 "auto-generated",
		"fixtures",	"testdata",	 "test_data",
		"__tests__",	"__mocks__",	 "__snapshots__",
		"__fixtures__", "__test__",	 "docs",
		"doc",		"documentation", "examples",
		"example",	"samples",	 "sample",
		"scripts",	"tools",	 "hack",
		"migrations",	"seeds",	 "e2e",
		"integration",	"locale",	 "locales",
		"i18n",		"l10n",		 "assets",
		"static",	"public",	 "media",
		"external",	"bin",
	};

	// Skip suffixes — non-source binaries, images, archives, etc.
	skip_suffixes_ = {
		".o",	 ".a",	    ".so",	".dll",	     ".dylib",
		".lib",	 ".exe",    ".bin",	".class",    ".wasm",
		".node", ".pyc",    ".pyo",	".pyd",	     ".png",
		".jpg",	 ".jpeg",   ".gif",	".ico",	     ".bmp",
		".tiff", ".webp",   ".svg",	".woff",     ".woff2",
		".ttf",	 ".eot",    ".otf",	".mp3",	     ".mp4",
		".avi",	 ".mov",    ".wav",	".flac",     ".ogg",
		".mkv",	 ".pdf",    ".doc",	".docx",     ".xls",
		".xlsx", ".ppt",    ".pptx",	".zip",	     ".tar",
		".gz",	 ".bz2",    ".xz",	".rar",	     ".7z",
		".jar",	 ".war",    ".map",	".pem",	     ".crt",
		".key",	 ".p12",    ".pb",	".avro",     ".parquet",
		".beam", ".elc",    ".rlib",	".coverage", ".prof",
		".db",	 ".sqlite", ".sqlite3", ".log",	     ".tmp",
		"~",
	};
	fast_extra_suffixes_ = {
		".min.js",
		".min.css",
	};

	// Skip filenames
	skip_filenames_ = {
		"package-lock.json", "yarn.lock",  "pnpm-lock.yaml",
		"Gemfile.lock",	     "Cargo.lock", ".DS_Store",
		"Thumbs.db",
	};

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
	if (active_skip_dirs_.find(dir_name) != active_skip_dirs_.end())
		return true;
	return false;
}

bool FilterPolicy::shouldSkipFile(const std::string &filename) const
{
	if (skip_filenames_.find(filename) != skip_filenames_.end())
		return true;
	return false;
}

bool FilterPolicy::shouldSkipSuffix(const std::string &ext) const
{
	auto it = skip_suffixes_.find(ext);
	if (it != skip_suffixes_.end())
		return true;
	if (mode_ == FAST) {
		auto fit = fast_extra_suffixes_.find(ext);
		if (fit != fast_extra_suffixes_.end())
			return true;
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
	const char *ext = strrchr(file_path, '.');
	if (!ext)
		return nullptr;

	// Skip minified/bundled JS
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
	if (strcmp(ext, ".mjs") == 0)
		return "javascript";
	if (strcmp(ext, ".ts") == 0)
		return "typescript";
	if (strcmp(ext, ".tsx") == 0)
		return "tsx";
	if (strcmp(ext, ".go") == 0)
		return "go";
	if (strcmp(ext, ".java") == 0)
		return "java";
	if (strcmp(ext, ".kt") == 0 || strcmp(ext, ".kts") == 0)
		return "kotlin";
	if (strcmp(ext, ".rb") == 0)
		return "ruby";
	if (strcmp(ext, ".scala") == 0)
		return "scala";

	return nullptr;
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
