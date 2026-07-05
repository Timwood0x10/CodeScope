#include "project_index.h"

namespace resolver
{

void ProjectSymbolIndex::addEntry(const IndexEntry &entry)
{
	std::lock_guard<std::mutex> lock(mutex_);
	name_index_[entry.name].push_back(entry);
	entries_.emplace_back(entry.name, entry);
}

const std::vector<IndexEntry> *
ProjectSymbolIndex::lookup(const std::string &name) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = name_index_.find(name);
	if (it != name_index_.end())
		return &it->second;
	return nullptr;
}

void ProjectSymbolIndex::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	name_index_.clear();
	entries_.clear();
}

} // namespace resolver
