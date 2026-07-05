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
	records_.push_back(std::move(rec));
	return records_.back().id;
}

const Record &SemanticUnit::getRecord(uint64_t id) const
{
	// Linear scan — the record count per file is small (< 10K).
	// If this becomes a bottleneck, add an id → index map.
	for (const auto &r : records_) {
		if (r.id == id)
			return r;
	}
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
