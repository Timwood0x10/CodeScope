#ifndef CODESCOPE_FINDING_H
#define CODESCOPE_FINDING_H

#include <string>
#include <vector>

namespace verify
{

/**
 * A single finding from an Integrity Verifier.
 * Each finding has evidence that can be traced back to the Knowledge Graph.
 */
struct Evidence {
	std::string entity_name; // e.g. "IncrementalIndex"
	std::string file_path; // e.g. "src/engine_incremental.cpp"
	int line = 0;
	std::string detail; // e.g. "0 callers, returns true, marked TODO"
};

struct Finding {
	std::string type; // e.g. "DeadCapability", "BrokenContract"
	std::string description; // Human-readable summary
	double confidence = 0.0; // 0.0 - 1.0
	std::vector<Evidence> evidence;
};

} // namespace verify

#endif // CODESCOPE_FINDING_H