#ifndef FILTER_POLICY_H
#define FILTER_POLICY_H

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
 * Centralized file filtering policy for CodeScope.
 *
 * Replaces ad-hoc skip_dirs / skip_suffixes scattered across
 * engine_index_project and engine_scanner. Supports:
 * - Normal / FAST mode filtering
 * - .codescopeignore file
 * - Per-language extension mapping
 * - Discovery stats (seen/skipped/candidate counts)
 */
class FilterPolicy {
    public:
	FilterPolicy();

	// ── Mode ─────────────────────────────────────────────────────
	enum Mode { NORMAL, FAST };
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
	bool shouldSkipFile(const std::string &filename) const;
	bool shouldSkipSuffix(const std::string &ext) const;
	bool isSourceFile(const std::string &path) const;

	// ── Language Detection ───────────────────────────────────────
	const char *detectLanguage(const char *file_path) const;

	// ── .codescopeignore ─────────────────────────────────────────
	bool loadIgnoreFile(const std::string &project_root);

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

	// Skip suffixes (always applied)
	std::unordered_set<std::string> skip_suffixes_;
	// FAST mode extra suffixes
	std::unordered_set<std::string> fast_extra_suffixes_;

	// Skip filenames
	std::unordered_set<std::string> skip_filenames_;

	// .codescopeignore patterns
	std::vector<std::string> ignore_patterns_;

	// Stats
	mutable Stats stats_;

	void buildActiveSets();
};

#endif // FILTER_POLICY_H
