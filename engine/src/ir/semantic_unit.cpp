#include "semantic_unit.h"

#include <algorithm>

namespace ir
{

// ── Record Management ──────────────────────────────────────────

uint64_t SemanticUnit::addRecord(RecordKind kind, const std::string &name,
      uint64_t parent_id, SourceRange loc)
{
 Record rec;
 rec.id = next_id_++;
 rec.kind = kind;
 rec.name = name;
 rec.parent_id = parent_id;
 rec.loc = loc;
 rec.file_path = file_path_;
 rec.language = language_;
 id_to_index_[rec.id] = records_.size();
 records_.push_back(std::move(rec));
 return records_.back().id;
}

uint64_t SemanticUnit::addRecord(RecordKind kind, const std::string &name,
       const std::string &qualified_name,
       uint64_t original_id, uint64_t parent_id,
       SourceRange loc)
{
 Record rec;
 rec.id = next_id_++;
 rec.kind = kind;
 rec.name = name;
 rec.qualified_name = qualified_name;
 rec.parent_id = parent_id;
 rec.loc = loc;
 rec.file_path = file_path_;
 rec.language = language_;
 id_to_index_[rec.id] = records_.size();
 // Store original_id → new_id mapping for parent_id reconstruction
 if (original_id > 0)
  id_to_index_[original_id] = records_.size();
 records_.push_back(std::move(rec));
 return records_.back().id;
}

const Record &SemanticUnit::getRecord(uint64_t id) const
{
	auto it = id_to_index_.find(id);
	if (it != id_to_index_.end())
		return records_[it->second];
	// Return the last record as sentinel (caller must check).
	// In practice, IDs are always valid.
	return records_.back();
}

// ── Query ──────────────────────────────────────────────────────

std::vector<size_t> SemanticUnit::findRecordsByKind(RecordKind kind) const
{
	std::vector<size_t> result;
	for (size_t i = 0; i < records_.size(); i++) {
		if (records_[i].kind == kind)
			result.push_back(i);
	}
	return result;
}

size_t SemanticUnit::findRecordByName(const std::string &name) const
{
	for (size_t i = 0; i < records_.size(); i++) {
		if (records_[i].name == name)
			return i;
	}
	return SIZE_MAX;
}

std::vector<size_t> SemanticUnit::getChildren(uint64_t parent_id) const
{
	std::vector<size_t> result;
	for (size_t i = 0; i < records_.size(); i++) {
		if (records_[i].parent_id == parent_id)
			result.push_back(i);
	}
	return result;
}

} // namespace ir
