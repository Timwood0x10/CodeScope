#include "filter_policy.h"
#include <cctype>
#include <cstdio>
#include <cstring>
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
		// ── AI assistant config directories ──
		".atomcode",
		".workbuddy",
		".gemini",
		".opencode",
		".cursh",
		// ── CI/CD pipeline configs ──
		".github",
		".circleci",
		".gitlab-ci",
		".bitbucket",
		".azure-pipelines",
		// ── Project planning / changelog ──
		"plan",
		"changelogs",
		// ── DevOps / infrastructure ──
		"infra",
		"deploy",
		"deployment",
		"docker",
		"kubernetes",
		"k8s",
		"helm",
		// ── VS Code container ──
		".devcontainer",
		// ── Runtime / intermediate output ──
		"Runtimelog",
		"runtimelog",
		"llvm_ir",
		// ── CMake fetched dependencies ──
		"_deps",
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
		// ── Source-bearing dirs rarely the focus of analysis ──
		// Skipped at ANY path depth to avoid 3-5x file count inflation
		// from deep-nested test/docs dirs (e.g. rustc's
		// tools/rust-analyzer/crates/*/src/*/tests/ nests at depth 7).
		// Trade-off: this WILL skip Java packages whose components
		// collide with these names (e.g. org/springframework/samples/
		// petclinic's "samples", src/main/java/.../test/...). Java is
		// the ONLY language with such a carve-out — when the indexer
		// detects a .java file it flips lang_context_ to "java", which
		// routes these names through java_protected_skip_dirs_ at
		// top-only (depth ≤ 3) in shouldSkipDir()/shouldSkipPath(),
		// protecting nested package namespaces. See README.md "Why
		// Java is the (only) exception". NOTE: CODESCOPE_EXCLUDE_PATHS
		// only ADDS exclude patterns; it CANNOT clear this list. The
		// prior top-only (depth ≤ 3) matching leaked deep-nested test
		// dirs, inflating node counts on monorepos (Lerna
		// packages/*/test/, Gradle subprojects/*/src/test/).
		"bin",
		"third_party",
		"thirdparty",
		"3rdparty",
		"vendor",
		"vendored",
		"bench",
		"benchmark",
		"benchmarks",
		// ── Test / docs / samples dirs (match at ANY depth) ──
		// Moved from top_only_skip_dirs_ so deep-nested test dirs
		// (the common case in Cargo workspaces, Lerna monorepos,
		// Gradle multi-module builds) are always skipped. Java users
		// who need these as package names: see comment above.
		//
		// EXCEPTION: Java projects keep the old top-only (depth ≤ 3)
		// behavior via java_protected_skip_dirs_ + the lang_context_
		// gate in shouldSkipPath(). Java is the only language whose
		// package namespaces collide with these names (samples/test/
		// examples/ as legitimate org/.../samples/petclinic package
		// components). All other languages (Rust, Go, Python, JS/TS,
		// C/C++, ...) nest test/ at any depth and expect it skipped.
		"test",
		"tests",
		"docs",
		"doc",
		"documentation",
		"examples",
		"example",
		"samples",
		"sample",
		"e2e",
		// NOTE: scripts / hack / migrations / seeds / integration /
		// locale(s) / i18n / l10n / assets / static / public / media /
		// external were MOVED OUT of this any-depth set into
		// top_only_skip_dirs_ below. They are ordinary business
		// directories outside the web-frontend convention
		// (src/integration/, pkg/scripts/, app/static/, db/migrations/
		// all hold real, first-party code), and the generated/asset
		// noise they were meant to exclude is already rejected by the
		// source-extension filter. Matching them at any depth silently
		// dropped source files — and therefore symbols and call edges
		// (a false negative the index cannot distinguish from code that
		// does not exist). They are still skipped at the project root
		// and the next two levels (depth <= 3).
	};

	// FAST mode skips even more — build/test artifacts that NORMAL
	// mode keeps (they are generated, rarely the focus of analysis,
	// and can dominate file counts on large repos). Everything in
	// normal_skip_dirs_ is skipped in both modes; this set is merged
	// into active_skip_dirs_ only when mode_ == FAST/STRICT (see
	// buildActiveSets()).
	fast_extra_skip_dirs_ = {
		// ── Frontend build output & generated code ──
		".output", // Next.js/Remix/Astro build output
		"storybook-static", // Storybook static build
		"__generated__", // GraphQL/Prisma/typed codegen output
		// ── Test reports (large, machine-generated) ──
		"playwright-report",
		"test-results",
		"allure-results",
		"allure-report",
		// ── CSS preprocessor caches ──
		".sass-cache",
		".scss-cache",
		// ── Runtime logs ──
		"logs",
		".logs",
	};

	// ── Directory prefixes — catches build_test, build_master, etc. ──
	skip_dir_prefixes_ = {
		"build_", "cmake-build-", "_build", "tools-", "tools_",
	};

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
		// ── Project configuration (never source code) ──
		".toml", // Cargo.toml, pyproject.toml
		".yaml", // CI/CD, docker-compose
		".yml", // Same as .yaml
		".json", // package.json, tsconfig
		".xml", // pom.xml, build configs
		".ini", // Configuration files
		".cfg", // Configuration files
		".conf", // Configuration files
		".properties", // Java properties
		".gradle", // Gradle build scripts
		// ── Documentation (never source code) ──
		".md", // README, CHANGELOG
		".markdown", // Extended markdown
		".rst", // reStructuredText
		".adoc", // AsciiDoc
		".tex", // LaTeX
		// ── Build system scripts (config, not source) ──
		".cmake", // CMake module scripts
		".make", // Makefile fragments
		// ── Text / metadata (not source) ──
		".txt", // requirements.txt, NOTICE
		".csv", // Data tables
		".tsv", // Data tables
		// ── LLVM intermediate output ──
		".ll", // LLVM IR text
		".bc", // LLVM bitcode
		".rmeta", // Rust metadata (intermediate)
		// ── C/C++ build intermediates ──
		".d", // GCC/Clang dependency files
		// ── API definition files (config, not source) ──
		".graphql", // GraphQL schemas
		".gql", // GraphQL shorthand
		".proto", // Protobuf definitions
		// ── DevOps / container configs ──
		".dockerfile", // Docker build files
		".service", // systemd unit files
		".socket", // systemd socket files
	};
	fast_extra_suffixes_ = {
		".min.js",
		".min.css",
	};

	// FAST mode extra exact filenames — linter/formatter/build caches
	// that NORMAL mode keeps. These are single files (not suffixes), so
	// they can't go into fast_extra_suffixes_; checked in
	// shouldSkipFile() only when mode_ == FAST.
	fast_extra_filenames_ = {
		".eslintcache", // ESLint incremental cache
		".stylelintcache", // Stylelint cache
		".prettiercache", // Prettier cache
		"tsconfig.tsbuildinfo", // TypeScript incremental build info
	};

	// FAST mode extra filename prefixes (e.g. build-info.*). Empty for
	// now — reserved for future additions; kept symmetric with the other
	// fast_extra_* sets so the FAST path is uniform.
	fast_extra_filename_prefixes_ = {};

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
		// ── Additional config / metadata filenames ──
		"config.toml",
		"setup.cfg",
		"pyproject.toml",
		"docker-compose.yml",
		"docker-compose.yaml",
		"Dockerfile",
		".gitconfig",
		".npmignore",
		".eslintignore",
		".prettierignore",
		".stylelintignore",
		"tsconfig.json",
		"jsconfig.json",
		".babelrc",
		".browserslistrc",
		".node-version",
		".python-version",
		".tool-versions",
		"Makefile",
		"CMakeLists.txt",
		"GNUMakefile",
	};

	// Skip filename prefixes — files whose name STARTS WITH one of these.
	// All entries MUST be lowercase — shouldSkipFile lowercases the input
	// before comparison, so entries are matched case-insensitively.
	skip_filename_prefixes_ = {
		".env.", // .env.local, .env.production, .env.development.local ...
		"docker-compose.", // docker-compose.dev.yml, docker-compose.prod.yaml
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
	lowercaseAll(fast_extra_filenames_);
	lowercaseAll(fast_extra_filename_prefixes_);
	lowercaseAll(skip_dir_suffixes_);
	lowercaseAll(skip_filenames_);
	lowercaseAll(skip_filename_prefixes_);
	lowercaseAll(skip_dir_prefixes_);

	// Top-only (depth <= kTopOnlyDepth) skip dirs — business-plausible
	// names that were previously matched at ANY depth. They are skipped
	// only within the first kTopOnlyDepth path components, which is where
	// the conventions they target actually live:
	//     <root>/public/ ...        <root>/assets/ ...
	//     <root>/scripts/ ...       <root>/migrations/ ...
	//     <root>/i18n/ ...          <root>/external/ ...
	// Beyond that they are overwhelmingly first-party code, and a false
	// negative from a silent skip is indistinguishable from code that does
	// not exist — the worst failure mode for a code graph. The generated
	// junk these names were meant to exclude (JSON/po/css/binary) is
	// already rejected by the source-extension filter, so the relaxation
	// costs little and restores symbols the index was dropping.
	// Applied to EVERY language (see the step-1b check in shouldSkipPath);
	// the Java carve-out below additionally protects Java package
	// namespaces for the names that remain any-depth.
	top_only_skip_dirs_ = {
		"scripts", "hack",    "migrations", "seeds",	"integration",
		"locale",  "locales", "i18n",	    "l10n",	"assets",
		"static",  "public",  "media",	    "external",
	};
	lowercaseAll(top_only_skip_dirs_);

	// Java-protected skip dirs — the SAME source-bearing names that
	// other languages get skipped at any depth (in normal_skip_dirs_),
	// but Java needs them gated to top-only (depth ≤ 3) because Java
	// package namespaces collide with these names (e.g. a legit
	// package org/springframework/samples/petclinic has "samples" as
	// a package component, NOT a docs folder). When lang_context_ ==
	// "java", shouldSkipPath() consults THIS set at depth ≤ 3 instead
	// of letting normal_skip_dirs_ clobber nested packages. This is
	// the ONLY language with such a carve-out — other languages
	// (Rust, Go, Python, JS/TS, C/C++, ...) nest test/ at any depth
	// and expect it skipped unconditionally. See the rant in
	// README.md "Why Java is the (only) exception".
	java_protected_skip_dirs_ = {
		"test",		 "tests",    "docs",	    "doc",
		"documentation", "examples", "example",	    "samples",
		"sample",	 "scripts",  "hack",	    "migrations",
		"seeds",	 "e2e",	     "integration", "locale",
		"locales",	 "i18n",     "l10n",	    "assets",
		"static",	 "public",   "media",	    "external",
		"vendor",	 "vendored", "bench",	    "benchmarks",
	};
	lowercaseAll(java_protected_skip_dirs_);

	buildActiveSets();
}

void FilterPolicy::buildActiveSets()
{
	active_skip_dirs_ = normal_skip_dirs_;
	if (mode_ == FAST) {
		// fast_extra_skip_dirs_ is FAST-exclusive: STRICT mode keeps its
		// whitelist-only semantics (detectLanguage gate) and must NOT
		// silently drop these dirs before the whitelist check. This
		// matches the FAST-only gating of fast_extra_filenames_ /
		// fast_extra_filename_prefixes_ in shouldSkipFile().
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
	if (active_skip_dirs_.find(lower) != active_skip_dirs_.end()) {
		// Java carve-out: the test/tests/docs/samples/... names are
		// in active_skip_dirs_ (so non-Java projects skip them at any
		// depth), but Java package namespaces collide with these
		// names (e.g. org/springframework/samples/petclinic). For Java
		// projects, defer these to the top-only (depth ≤ 3) check in
		// shouldSkipPath() via java_protected_skip_dirs_, so nested
		// package components are NOT clobbered. See README.md "Why
		// Java is the (only) exception".
		if (lang_context_ == "java" &&
		    java_protected_skip_dirs_.find(lower) !=
			    java_protected_skip_dirs_.end()) {
			return false;
		}
		return true;
	}
	// Prefix match — catches build_test, build_master, etc.
	if (shouldSkipDirPrefix(lower))
		return true;
	return false;
}

bool FilterPolicy::shouldSkipDirPrefix(const std::string &dir_name) const
{
	for (const auto &pfx : skip_dir_prefixes_) {
		if (dir_name.size() >= pfx.size() &&
		    dir_name.compare(0, pfx.size(), pfx) == 0)
			return true;
	}
	return false;
}

bool FilterPolicy::isJavaProtectedDir(const std::string &dir_name) const
{
	std::string lower = dir_name;
	for (auto &c : lower)
		c = static_cast<char>(std::tolower(c));
	return java_protected_skip_dirs_.find(lower) !=
	       java_protected_skip_dirs_.end();
}

bool FilterPolicy::shouldSkipFile(const std::string &filename) const
{
	// Case-insensitive: lowercase before lookup so .ENV.LOCAL matches.
	std::string lower = filename;
	for (auto &c : lower)
		c = static_cast<char>(std::tolower(c));
	if (skip_filenames_.find(lower) != skip_filenames_.end())
		return true;
	// FAST-only exact filenames (linter/build caches, tsbuildinfo).
	if (mode_ == FAST) {
		if (fast_extra_filenames_.find(lower) !=
		    fast_extra_filenames_.end())
			return true;
		for (const auto &pfx : fast_extra_filename_prefixes_) {
			if (lower.size() >= pfx.size() &&
			    lower.compare(0, pfx.size(), pfx) == 0)
				return true;
		}
	}
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
