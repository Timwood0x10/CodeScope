#include "filter_policy.h"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

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
