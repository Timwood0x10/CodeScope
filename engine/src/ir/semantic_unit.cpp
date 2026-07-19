#include "semantic_unit.h"

#include <algorithm>

namespace ir
{

// ── Record Management ──────────────────────────────────────────

uint64_t SemanticUnit::addRecord(RecordKind kind, const std::string &name,
				 uint64_t parent_id, SourceRange loc, int arity,
				 bool is_static, int visibility)
{
	Record rec;
	rec.id = next_id_++;
	rec.kind = kind;
	rec.name = name;
	rec.parent_id = parent_id;
	rec.loc = loc;
	rec.file_path = file_path_;
	rec.language = language_;
	rec.arity = arity;
	rec.is_static = is_static;
	rec.visibility = visibility;
	id_to_index_[rec.id] = records_.size();
	records_.push_back(std::move(rec));
	return records_.back().id;
}

uint64_t SemanticUnit::addTypedRecord(RecordKind kind, const std::string &name,
				      const std::string &type_name,
				      uint64_t parent_id, SourceRange loc,
				      int visibility)
{
	Record rec;
	rec.id = next_id_++;
	rec.kind = kind;
	rec.name = name;
	rec.type_name = type_name;
	rec.parent_id = parent_id;
	rec.loc = loc;
	rec.file_path = file_path_;
	rec.language = language_;
	rec.visibility = visibility;
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

const Record *SemanticUnit::getRecord(uint64_t id) const
{
	auto it = id_to_index_.find(id);
	if (it != id_to_index_.end())
		return &records_[it->second];
	// Not found: return nullptr instead of dereferencing records_.back()
	// on a possibly-empty container (undefined behaviour). Callers must
	// check the return value before dereferencing.
	return nullptr;
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

bool SemanticUnit::setCallReference(uint64_t record_id,
				    uint64_t ref_original_id)
{
	auto it = id_to_index_.find(record_id);
	if (it == id_to_index_.end())
		return false;
	records_[it->second].ref_original_id = ref_original_id;
	return true;
}

bool SemanticUnit::setCallKind(uint64_t record_id, int kind)
{
	auto it = id_to_index_.find(record_id);
	if (it == id_to_index_.end())
		return false;
	records_[it->second].call_kind = static_cast<CallKind>(kind);
	return true;
}

bool SemanticUnit::setCallStrategy(uint64_t record_id,
				   const std::string &strategy)
{
	auto it = id_to_index_.find(record_id);
	if (it == id_to_index_.end())
		return false;
	records_[it->second].resolve_strategy = strategy;
	return true;
}

} // namespace ir
