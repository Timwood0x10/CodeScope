#ifndef CODESCOPE_VERIFY_DOCUMENTATION_DRIFT_H
#define CODESCOPE_VERIFY_DOCUMENTATION_DRIFT_H

#include <cstdint>
#include <string>
#include <vector>

#include "../store/store.h"

namespace verify
{

// Severity for documentation drift findings (1 = warning).
inline constexpr int kDriftSeverityDoc = 1;

// Confidence score stamped on DocumentationDrift findings persisted to
// the finding table. Reflects that README parsing is heuristic-based.
inline constexpr double kDriftConfidenceDoc = 0.8;

// DriftItem represents a single documentation drift finding.
struct DriftItem {
	std::string type; // e.g. "DocumentationDrift"
	int severity = 0;
	std::string subject; // language name that is missing
	std::string detail; // human-readable description
};

// LanguageClaim represents a language mentioned in the README.
struct LanguageClaim {
	std::string
		canonical; // entity.language column value ("cpp", "rust", ...)
	std::string display; // human-readable name ("C++", "Rust", ...)
	size_t mention_count = 0; // how many times it appears in README
};

// Detect documentation drift between README claims and actual code.
//
// Reads the document table (type=0 = README) for the given project,
// extracts language mentions, and cross-references with the entity
// table's language column. Any language claimed in the README but
// with zero entities in the codebase is reported as a drift finding.
//
// @param store       The GraphStore to query.
// @param project_id  The project to check.
// @return            Vector of DriftItem for each missing language.
std::vector<DriftItem> detectDocumentationDrift(store::GraphStore &store,
						uint64_t project_id);

// Extract language claims from README text. Scans for language name
// patterns (C++, Python, Go, Rust, JavaScript, TypeScript, Java) and
// returns canonical language identifiers with mention counts.
//
// @param readme_text  The README content to scan.
// @return             Vector of LanguageClaim, one per detected language.
std::vector<LanguageClaim>
extractLanguageClaims(const std::string &readme_text);

// Count entities in the entity table for a given language.
//
// @param store       The GraphStore to query.
// @param project_id  The project scope.
// @param language    The canonical language name ("cpp", "rust", ...).
// @return            Number of entities with the given language.
int64_t countEntitiesByLanguage(store::GraphStore &store, uint64_t project_id,
				const std::string &language);

} // namespace verify

#endif // CODESCOPE_VERIFY_DOCUMENTATION_DRIFT_H
