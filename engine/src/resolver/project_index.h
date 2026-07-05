#ifndef PROJECT_INDEX_H
#define PROJECT_INDEX_H

#include <string>
#include <unordered_map>
#include <vector>

#include "../ir/ir.h"

namespace resolver
{

// ─── Symbol Index Entry ─────────────────────────────────────────
//
// A single entry in the project-wide symbol index.
// Populated by pre-scanning all source files before translation.

struct IndexEntry {
	std::string name;
	std::string file_path;
	std::string module_path; // e.g. "src/util" from "src/util/helper.c"
	ir::NodeKind kind;
	ir::SourceLocation loc;
	bool is_static = false; // static functions limited to file scope
};

// ─── Project Symbol Index ──────────────────────────────────────
//
// In-memory, project-wide symbol lookup table.
// Built by a pre-scan pass before main translation begins.
// Not backed by SQL — purely an unordered_map for O(1) lookup.

class ProjectSymbolIndex {
    public:
	// Add a single symbol entry to the index.
	void addEntry(const IndexEntry &entry);

	// Look up all entries with the given name.
	// Returns empty vector if no entries found.
	const std::vector<IndexEntry> *lookup(const std::string &name) const;

	// Clear all entries (for re-indexing).
	void clear();

	// Return total number of indexed symbols.
	size_t size() const
	{
		return entries_.size();
	}

	// Check if the index has been populated.
	bool empty() const
	{
		return name_index_.empty();
	}

    private:
	// Primary index: name → entries
	std::unordered_map<std::string, std::vector<IndexEntry> > name_index_;

	// Flat list of all entries (for iteration / stats)
	std::vector<std::pair<std::string, IndexEntry> > entries_;
};

} // namespace resolver

#endif // PROJECT_INDEX_H
