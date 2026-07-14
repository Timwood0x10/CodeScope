#include "contract.h"
#include <cstdio>
#include <cstring>

namespace model
{

namespace
{

// Case-insensitive substring search mirroring SQLite's default LIKE
// behaviour for ASCII. Used to replicate "name LIKE '%TODO%'" etc.
bool containsCI(const std::string &haystack, const char *needle)
{
	if (!needle || !*needle)
		return true;
	auto lower = [](char c) -> char {
		return (c >= 'A' && c <= 'Z') ?
			       static_cast<char>(c - 'A' + 'a') :
			       c;
	};
	const size_t nlen = std::strlen(needle);
	if (nlen > haystack.size())
		return false;
	for (size_t i = 0; i + nlen <= haystack.size(); ++i) {
		size_t j = 0;
		for (; j < nlen; ++j) {
			if (lower(haystack[i + j]) != lower(needle[j]))
				break;
		}
		if (j == nlen)
			return true;
	}
	return false;
}

} // namespace

ContractPlugin::ContractPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult ContractPlugin::build(uint64_t project_id, const ModelContext &ctx)
{
	ModelResult r;
	r.plugin_name = "Contract";

	int64_t contracts = 0;

	// Extract contracts from README documents (pre-fetched in ctx).
	// Look for contract keywords using case-sensitive search, matching
	// the original std::string::find-based logic.
	for (const auto &doc : ctx.documents) {
		const std::string &text = doc.content;
		static constexpr const char *keywords[] = {
			"thread safe", "thread-safe", "ThreadSafe",
			"memory safe", "memory-safe", "MemorySafe",
			"zero-copy",   "zero copy",   "lock-free",
			"lock free",   "not safe",    "unsafe"
		};
		for (auto *kw : keywords) {
			if (text.find(kw) != std::string::npos) {
				store_->insertContract(project_id, kw, "readme",
						       kw, doc.file_path, 0);
				++contracts;
			}
		}
	}

	// Scan entity names for TODO/FIXME/HACK/XXX markers.
	// Preserves the original SQL operator precedence:
	//   (project_id = ? AND kind IN (0,1) AND name LIKE '%TODO%')
	//   OR name LIKE '%FIXME%' OR name LIKE '%HACK%' OR name LIKE '%XXX%'
	// The ctx is project-scoped, so the project_id filter is implicit.
	// The kind filter only applies to the TODO clause (as in the
	// original), not to FIXME/HACK/XXX.
	int todo_count = 0;
	for (uint64_t eid : ctx.entity_ids_ordered) {
		if (todo_count >= kMaxTodoEntities)
			break;
		auto it = ctx.entities_by_id.find(eid);
		if (it == ctx.entities_by_id.end())
			continue;
		const EntityInfo &e = it->second;
		bool match = false;
		if ((e.kind == kEntityKindFunction ||
		     e.kind == kEntityKindMethod) &&
		    containsCI(e.name, "TODO")) {
			match = true;
		} else if (containsCI(e.name, "FIXME") ||
			   containsCI(e.name, "HACK") ||
			   containsCI(e.name, "XXX")) {
			match = true;
		}
		if (!match)
			continue;
		store_->insertContract(project_id,
				       std::string("TODO: ") + e.name,
				       "comment", e.name, e.file_path, 0);
		++contracts;
		++todo_count;
	}

	r.items_created = contracts;
	return r;
}

} // namespace model
