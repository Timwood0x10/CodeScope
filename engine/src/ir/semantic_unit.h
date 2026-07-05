#ifndef SEMANTIC_UNIT_H
#define SEMANTIC_UNIT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir
{

/**
 * Source location range within a file.
 * All fields are 0-based (matching tree-sitter TSPoint convention).
 */
struct SourceRange {
	uint32_t start_row = 0;
	uint32_t start_col = 0;
	uint32_t end_row = 0;
	uint32_t end_col = 0;

	bool isValid() const
	{
		return start_row <= end_row ||
		       (start_row == end_row && start_col <= end_col);
	}
};

/**
 * Kind of a semantic record.
 * Only semantically meaningful nodes get a record — structural wrappers
 * (blocks, parentheses, expression statements) are elided during visiting.
 */
enum class RecordKind : uint8_t {
	Function,
	Method,
	Class,
	Interface,
	Enum,
	TypeAlias,
	Variable,
	Field,
	Parameter,
	CallExpr,
	MemberExpr,
	Import,
	Export,
	Literal,
	Comment,
	TranslationUnit,
};

/**
 * A single semantic fact extracted from the AST.
 * No pointers to other records — hierarchy is expressed via parent_id.
 * No children vector — tree structure is reconstructed from parent_id links.
 */
struct Record {
	uint64_t id = 0;
	RecordKind kind;
	std::string name;
	std::string qualified_name;
	uint64_t parent_id = 0; // 0 = top-level (child of TranslationUnit)
	SourceRange loc;
	std::string file_path;
	std::string language;
};

/**
 * Lightweight container for semantic facts extracted from one source file.
 *
 * Design principles:
 * - Flat vector of records (no tree, no Node objects, no children pointers)
 * - Parent-child relationships via parent_id (0 = root)
 * - Contiguous memory, cache-friendly iteration
 * - No heap-allocated per-node metadata (no std::vector per record)
 *
 * Typical size: ~50-200 bytes per record, ~50 KB for a 1000-line file.
 */
class SemanticUnit {
    public:
	SemanticUnit() = default;

	// ── Record Management ─────────────────────────────────────

	/** Add a record and return its assigned ID. */
	uint64_t addRecord(RecordKind kind, const std::string &name,
			   uint64_t parent_id, SourceRange loc);

	/** Accessors. */
	const Record &getRecord(uint64_t id) const;
	const std::vector<Record> &allRecords() const
	{
		return records_;
	}
	size_t size() const
	{
		return records_.size();
	}
	bool empty() const
	{
		return records_.empty();
	}

	// ── Metadata ───────────────────────────────────────────────

	const std::string &filePath() const
	{
		return file_path_;
	}
	void setFilePath(const std::string &path)
	{
		file_path_ = path;
	}

	const std::string &language() const
	{
		return language_;
	}
	void setLanguage(const std::string &lang)
	{
		language_ = lang;
	}

	// ── Query ──────────────────────────────────────────────────

	/** Find records by kind. Returns indices into records_. */
	std::vector<size_t> findRecordsByKind(RecordKind kind) const;

	/** Find a record by name (first match, or SIZE_MAX if not found). */
	size_t findRecordByName(const std::string &name) const;

	/** Get children of a record (records whose parent_id == id). */
	std::vector<size_t> getChildren(uint64_t parent_id) const;

    private:
	std::vector<Record> records_;
	std::unordered_map<uint64_t, size_t> id_to_index_; // record.id → index in records_
	std::string file_path_;
	std::string language_;
	uint64_t next_id_ = 1;
};

} // namespace ir

#endif // SEMANTIC_UNIT_H
