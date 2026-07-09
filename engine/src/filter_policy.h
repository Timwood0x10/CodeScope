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
	enum Mode { NORMAL, FAST, STRICT };
	void setMode(Mode m)
	{
		mode_ = m;
	}
	Mode mode() const
	{
		return mode_;
	}

	// ── Configuration ────────────────────────────────────────────
	void setLanguageFilter(const std::string &filter);
	bool isLanguageAccepted(const std::string &lang) const;

	// ── File/Dir Checks ──────────────────────────────────────────
	bool shouldSkipDir(const std::string &dir_name) const;
	bool shouldSkipDirPrefix(const std::string &dir_name) const;
	bool shouldSkipFile(const std::string &filename) const;
	bool shouldSkipSuffix(const std::string &ext) const;
	bool isSourceFile(const std::string &path) const;

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

	// Skip dirs: Normal mode
	std::unordered_set<std::string> normal_skip_dirs_;
	// Skip dirs: FAST mode adds these
	std::unordered_set<std::string> fast_extra_skip_dirs_;
	// Combined for current mode
	std::unordered_set<std::string> active_skip_dirs_;

	// Skip suffixes (always applied, case-insensitive) — for FILES
	std::unordered_set<std::string> skip_suffixes_;
	// FAST mode extra suffixes
	std::unordered_set<std::string> fast_extra_suffixes_;
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
			     std::string::const_iterator si);
};

#endif // FILTER_POLICY_H
