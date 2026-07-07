#include "project_resolver.h"

#include <algorithm>
#include <filesystem>

namespace resolver
{

ResolutionResult ProjectResolver::resolve(const std::string &name,
					  const std::string &file_path,
					  const ir::Node *context)
{
	ResolutionResult result;
	result.name = name;
	result.resolver_name = "project_resolver";

	if (!index_ || index_->empty()) {
		result.status = ResolutionStatus::NotFound;
		return result;
	}

	const auto *candidates = index_->lookup(name);
	if (!candidates || candidates->empty()) {
		result.status = ResolutionStatus::NotFound;
		return result;
	}

	// Score each candidate
	struct Scored {
		const IndexEntry *entry;
		int score;
	};
	std::vector<Scored> scored;
	scored.reserve(candidates->size());

	for (const auto &entry : *candidates) {
		// Skip static declarations that are not in the caller's file
		if (entry.is_static && entry.file_path != file_path)
			continue;

		int score = rankCandidate(entry, file_path);
		scored.push_back({ &entry, score });
	}

	if (scored.empty()) {
		result.status = ResolutionStatus::NotFound;
		return result;
	}

	// Sort by score descending
	std::sort(scored.begin(), scored.end(),
		  [](const Scored &a, const Scored &b) {
			  return a.score > b.score;
		  });

	// Build ResolvedSymbol results
	for (const auto &s : scored) {
		ResolvedSymbol rs;
		rs.name = s.entry->name;
		rs.file_path = s.entry->file_path;
		rs.kind = s.entry->kind;
		rs.loc = s.entry->loc;
		rs.rank_score = s.score;
		result.candidates.push_back(rs);
	}

	// Determine status:
	// If best score > 0 and strictly higher than second best → Resolved
	// If best score == second best → Ambiguous
	// Otherwise → Resolved (single candidate)
	if (scored.size() == 1) {
		result.status = ResolutionStatus::Resolved;
	} else if (scored[0].score > scored[1].score) {
		result.status = ResolutionStatus::Resolved;
	} else {
		result.status = ResolutionStatus::Ambiguous;
		result.error_message =
			"Multiple candidates with equal rank for '" + name +
			"'";
	}

	return result;
}

int ProjectResolver::rankCandidate(const IndexEntry &candidate,
				   const std::string &caller_file) const
{
	int score = 0;

	// Same directory: strong signal (likely in the same module)
	std::string caller_dir = extractModule(caller_file);
	std::string candidate_dir = extractModule(candidate.file_path);
	if (!caller_dir.empty() && caller_dir == candidate_dir)
		score += 3;

	// Same file: already handled by local resolver, but if we got here
	// it means local didn't find it. Still score it.
	if (caller_file == candidate.file_path)
		score += 1;

	// Not static: more likely to be visible across files
	if (!candidate.is_static)
		score += 1;

	// Shorter path distance = more likely related
	// (heuristic: files in nearby directories are more likely related)
	try {
		std::filesystem::path cp(caller_file);
		std::filesystem::path dp(candidate.file_path);
		int depth_diff = 0;
		auto ci = cp.begin(), di = dp.begin();
		while (ci != cp.end() && di != dp.end() && *ci == *di) {
			++ci;
			++di;
		}
		// Count remaining components as distance
		int remaining = 0;
		for (; ci != cp.end(); ++ci)
			remaining++;
		for (; di != dp.end(); ++di)
			remaining++;
		if (remaining <= 1)
			score += 2;
		else if (remaining <= 3)
			score += 1;
	} catch (const std::exception &e) {
	  fprintf(stderr,
	   "RESOLVER: rankCandidate filesystem error: %s\n",
	   e.what());
	 }

	 return score;
	}

	std::string ProjectResolver::extractModule(const std::string &file_path)
	{
	 try {
	  std::filesystem::path p(file_path);
	  if (p.has_parent_path()) {
	   return p.parent_path().lexically_normal().string();
	  }
	 } catch (const std::exception &e) {
	  fprintf(stderr,
	   "RESOLVER: extractModule filesystem error: %s\n",
	   e.what());
	 }
	return "";
}

} // namespace resolver
