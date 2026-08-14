#ifndef FILTER_POLICY_H
#define FILTER_POLICY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
 * Gitignore-style pattern rule.
 */
struct GitignoreRule {
	std::string pattern; // raw pattern (after stripping ! and trailing /)
	bool negate = false; // starts with '!'
	bool dir_only = false; // ends with '/'
	bool anchored = false; // starts with '/'
	bool has_star = false; // contains * or **
};

/**
 * Centralized file filtering policy for CodeScope.
 *
 * Replaces ad-hoc skip_dirs / skip_suffixes scattered across
 * engine_index_project and engine_scanner. Supports:
 * - Normal / FAST mode filtering
 * - .codescopeignore file
 * - .gitignore pattern matching
 * - Per-language extension mapping
 * - Discovery stats (seen/skipped/candidate counts)
 */
class FilterPolicy {
    public:
	FilterPolicy();

	// ── Mode ─────────────────────────────────────────────────────
	// Windows SDK (windows.h) defines STRICT / FAST as macros; undef them
	// so the enum values below compile (same guard as engine_index_project.cpp).
#ifdef STRICT
#undef STRICT
#endif
#ifdef FAST
#undef FAST
#endif
	enum Mode { NORMAL, FAST, STRICT };
	void setMode(Mode m)
	{
		mode_ = m;
		// active_skip_dirs_ is mode-dependent (FAST/STRICT add
		// fast_extra_skip_dirs_). Rebuild it so a mode switch made
		// AFTER construction (e.g. from CODESCOPE_INDEX_MODE env) is
		// reflected immediately; otherwise fast_extra_skip_dirs_ would
		// silently never take effect.
		buildActiveSets();
	}
	Mode mode() const
	{
		return mode_;
	}

	// ── Configuration ────────────────────────────────────────────
	void setLanguageFilter(const std::string &filter);
	bool isLanguageAccepted(const std::string &lang) const;

	// ── Language context (for path-component collision policy) ───
	// Set by the indexer when it detects the project's primary
	// language. "java" relaxes the test/docs/samples skip rule to
	// top-only (depth ≤ 3) so Java package components like
	// org/springframework/samples/petclinic are NOT falsely skipped.
	// All other languages keep these names skipped at ANY depth.
	// See shouldSkipPath() and the rant in README.md "Why Java is
	// the (only) exception".
	void setLangContext(const std::string &lang)
	{
		lang_context_ = lang;
	}
	const std::string &langContext() const
	{
		return lang_context_;
	}

	// ── File/Dir Checks ──────────────────────────────────────────
	bool shouldSkipDir(const std::string &dir_name) const;
	bool shouldSkipDirPrefix(const std::string &dir_name) const;
	bool shouldSkipFile(const std::string &filename) const;
	bool shouldSkipSuffix(const std::string &ext) const;
	bool isSourceFile(const std::string &path) const;

	/// Whether a directory basename is in the Java-protected set
	/// (test/tests/docs/example/samples/vendor/...). For Java projects
	/// these names are deferred to a top-only (depth ≤ 3) check so nested
	/// package namespaces (org/springframework/samples/petclinic) are not
	/// clobbered; for non-Java projects they are skipped at any depth.
	/// Exposed so the indexer can pre-detect .java files BEFORE the walk
	/// and flip lang_context_ to "java" — otherwise the first .java file
	/// (which may live under an example/samples/... dir) is never reached
	/// because that dir is skipped while lang_context_ is still empty
	/// (chicken-and-egg: Java projects index 0 files).
	bool isJavaProtectedDir(const std::string &dir_name) const;

	// ── Path-based check (gitignore-aware, any depth) ────────────
	// Check ALL path components against skip_dirs AND full path
	// against .gitignore / .codescopeignore patterns.
	// @param rel_path  Path relative to project root.
	// @param is_dir    Whether this entry is a directory.
	bool shouldSkipPath(const std::string &rel_path, bool is_dir) const;

	// ── Bundle-directory suffix check ───────────────────────────
	// True if a directory's basename ends with a known bundle suffix
	// (.app, .framework, .xcodeproj, ...). Case-insensitive.
	bool shouldSkipDirSuffix(const std::string &dir_name) const;

	// ── Consolidated entry check (single source of truth) ───────
	// Applies the FULL filtering pipeline in one call:
	//   1. Path-component skip_dirs (any depth) — node_modules, .venv, ...
	//   2. .gitignore pattern matching
	//   3. .codescopeignore raw patterns
	//   4. Filename exact skip (package-lock.json, .env, .DS_Store, ...)
	//   5. Filename prefix skip (.env.*, yarn-error.log.*)
	//   6. Suffix skip — case-insensitive (.EXE == .exe) — binaries,
	//      archives, media, lock files, generated artifacts
	// Use this instead of calling shouldSkipPath/shouldSkipFile/
	// shouldSkipSuffix separately so no check is silently missed.
	// @param rel_path  Path relative to project root.
	// @param is_dir    Whether this entry is a directory.
	bool shouldSkipEntry(const std::string &rel_path, bool is_dir) const;

	// ── Language Detection ───────────────────────────────────────
	const char *detectLanguage(const char *file_path) const;

	// ── Ignore Files ─────────────────────────────────────────────
	// Load .codescopeignore patterns from project root.
	bool loadIgnoreFile(const std::string &project_root);
	// Load .gitignore patterns from project root.
	bool loadGitignore(const std::string &project_root);
	// Load CODESCOPE_EXCLUDE_PATHS env var — comma-separated glob
	// patterns (e.g. "test/*,docs/*,vendor/*,third_party/*") that
	// extend the built-in skip list at index time. Useful for trimming
	// non-core directories on very large projects to keep graph_nodes
	// under the FTS threshold. Defaults are NOT applied — only patterns
	// the user explicitly sets are loaded.
	// @return true if at least one pattern was loaded.
	bool loadExcludeEnv();

	// ── Gitignore-Only Check ────────────────────────────────────
	// Check a relative path against loaded .gitignore rules only
	// (without skip_dirs/suffix/language filters).
	// Returns true if the path matches a gitignore pattern.
	bool isGitignoreMatch(const std::string &rel_path, bool is_dir) const
	{
		return gitignoreMatches(gitignore_rules_, rel_path, is_dir);
	}

	// ── Stats ────────────────────────────────────────────────────
	struct Stats {
		uint64_t seen_dirs = 0;
		uint64_t seen_files = 0;
		uint64_t skipped_dirs = 0;
		uint64_t skipped_files = 0;
		uint64_t skipped_suffix = 0;
		uint64_t skipped_lang = 0;
		uint64_t skipped_ignore = 0;
		uint64_t candidate_files = 0;
	};
	Stats &stats()
	{
		return stats_;
	}
	const Stats &stats() const
	{
		return stats_;
	}
	void printStats() const;

    private:
	Mode mode_ = NORMAL;
	std::unordered_set<std::string> lang_filter_set_;
	bool has_lang_filter_ = false;
	// Primary language of the project being indexed ("java" / "rust" /
	// "" = unset). Drives the test/docs skip policy in
	// shouldSkipPath(): "java" → top-only (depth ≤ 3) via
	// java_protected_skip_dirs_; all other → any-depth via
	// normal_skip_dirs_.
	std::string lang_context_;

	// Skip dirs: Normal mode
	std::unordered_set<std::string> normal_skip_dirs_;
	// Skip dirs: FAST mode adds these
	std::unordered_set<std::string> fast_extra_skip_dirs_;
	// Combined for current mode
	std::unordered_set<std::string> active_skip_dirs_;

	// Top-only skip dirs: source-bearing but rarely the focus of
	// analysis (test/, docs/, vendor/, bench/, samples/, ...).
	// Matched ONLY against the first path component of rel_path,
	// so they catch <root>/test/, <root>/docs/ etc. without
	// clobbering same-named Java package components like
	// org/springframework/samples/petclinic (the "samples"
	// component must NOT be skipped). See shouldSkipPath().
	std::unordered_set<std::string> top_only_skip_dirs_;

	// Java-protected skip dirs: same set as the test/docs/samples
	// names in normal_skip_dirs_, but applied ONLY when
	// lang_context_ == "java" and ONLY against the first 3 path
	// components. Lets Java keep the old top-only behavior
	// (protecting nested package namespaces) while every other
	// language gets these names skipped at any depth via
	// normal_skip_dirs_.
	std::unordered_set<std::string> java_protected_skip_dirs_;

	// Skip suffixes (always applied, case-insensitive) — for FILES
	std::unordered_set<std::string> skip_suffixes_;
	// FAST mode extra suffixes
	std::unordered_set<std::string> fast_extra_suffixes_;
	// FAST mode extra exact filenames (e.g. linter caches) — checked
	// in shouldSkipFile() only when mode_ == FAST.
	std::unordered_set<std::string> fast_extra_filenames_;
	// FAST mode extra filename prefixes — checked in shouldSkipFile()
	// only when mode_ == FAST.
	std::unordered_set<std::string> fast_extra_filename_prefixes_;
	// Skip dir suffixes — directory ENTRIES whose name ends with one of
	// these (e.g. MyApp.app, GLFW.framework, proj.xcodeproj). These are
	// bundle/package directories on macOS / IDE project dirs that must be
	// skipped wholesale to avoid recursing into binary payloads.
	std::unordered_set<std::string> skip_dir_suffixes_;

	// Skip filenames (exact, case-sensitive)
	std::unordered_set<std::string> skip_filenames_;
	// Skip filename prefixes (e.g. ".env" -> .env, .env.local, .env.production)
	std::unordered_set<std::string> skip_filename_prefixes_;

	// Skip directory prefixes — catches build_test, build_master, etc.
	std::unordered_set<std::string> skip_dir_prefixes_;

	// .codescopeignore patterns (raw lines)
	std::vector<std::string> ignore_patterns_;
	// .gitignore patterns (parsed rules)
	std::vector<GitignoreRule> gitignore_rules_;
	// CODESCOPE_EXCLUDE_PATHS patterns (comma-separated globs from env).
	// Glob-matched against the relative path from project root.
	std::vector<std::string> exclude_patterns_;

	// Stats
	mutable Stats stats_;

	void buildActiveSets();

	// Gitignore pattern matching helpers (static, reusable)
	static bool gitignoreMatches(const std::vector<GitignoreRule> &rules,
				     const std::string &rel_path, bool is_dir);
	static bool globMatch(const std::string &pattern,
			      const std::string &str);
	static bool globImpl(const std::string &p, const std::string &s,
			     std::string::const_iterator pi,
			     std::string::const_iterator si, int depth = 0);
};

#endif // FILTER_POLICY_H
