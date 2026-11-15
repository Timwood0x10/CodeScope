// Callable identity: what a bare name names, not how many rows it hit.
//
// Extracted from query_engine.cpp because getCallers/getCallees are
// grandfathered at their size (code_rules §1.1) and every name-based tool
// needs the same answer to "is this one symbol or several?".
#include "query_engine_identity.h"

#include <sqlite3.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/json_writer.h"
#include "util/path_util.h"

namespace query
{
namespace
{

/// The entity columns that decide callable identity.
struct Row {
	int64_t id = 0;
	int kind = 0;
	std::string language;
	std::string qualified_name;
	std::string signature;
	std::string name;
	std::string file_path;
	int64_t start_row = 0;
	int64_t start_col = 0;
};

std::string ColumnText(sqlite3_stmt *stmt, int column)
{
	const char *text = reinterpret_cast<const char *>(
		sqlite3_column_text(stmt, column));
	return text ? text : "";
}

/// The language family a callable belongs to, as engine_views::kSameCallable
/// defines it: a .h declaration is labelled 'cpp' and its .c definition 'c', so
/// exact spelling would keep the pair apart in every C project.
std::string LanguageFamily(const std::string &language)
{
	if (language == "c" || language == "cpp" || language == "c++")
		return "c-family";
	return language;
}

/// Mirrors engine_views::kSameCallable. Keeping the two spellings equal is the
/// point: symbol_context and find_callers must not disagree about how many
/// callables a name has — that disagreement is what this file exists to end.
std::string GroupKey(const Row &row)
{
	if ((row.kind == 0 || row.kind == 1) && !row.signature.empty())
		return "callable|" + LanguageFamily(row.language) + '|' +
		       row.qualified_name + '|' + row.signature;
	return "row|" + std::to_string(row.id);
}

} // namespace

std::vector<CallableGroup> resolveCallableGroups(sqlite3 *db,
						 uint64_t project_id,
						 const char *name,
						 const std::string &file_filter,
						 std::vector<int64_t> &rows)
{
	std::vector<CallableGroup> groups;
	rows.clear();
	if (!db || !name || !*name)
		return groups;

	const bool has_filter = !file_filter.empty();
	std::string sql =
		"SELECT id, kind, COALESCE(language,''), "
		"       COALESCE(NULLIF(qualified_name,''),name), "
		"       COALESCE(callable_signature,''), name, file_path, "
		"       start_row, start_col "
		"FROM entity WHERE project_id=? AND (name=? OR qualified_name=?)";
	if (has_filter)
		sql += " AND file_path LIKE ?";
	// A definition represents its callable better than the declaration that
	// names it, so the first row of a group is the one shown as the candidate.
	sql += " ORDER BY is_definition DESC, id";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		std::fprintf(stderr,
			     "[module=query, method=resolveCallableGroups] "
			     "prepare failed: %s\n",
			     sqlite3_errmsg(db));
		return groups;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
	if (has_filter) {
		const auto patterns = util::lookupPatterns(file_filter);
		sqlite3_bind_text(stmt, 4, patterns[0].c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, patterns[1].c_str(), -1,
				  SQLITE_TRANSIENT);
	}

	std::unordered_map<std::string, size_t> seen;
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		Row row;
		row.id = sqlite3_column_int64(stmt, 0);
		row.kind = sqlite3_column_int(stmt, 1);
		row.language = ColumnText(stmt, 2);
		row.qualified_name = ColumnText(stmt, 3);
		row.signature = ColumnText(stmt, 4);
		row.name = ColumnText(stmt, 5);
		row.file_path = ColumnText(stmt, 6);
		row.start_row = sqlite3_column_int64(stmt, 7);
		row.start_col = sqlite3_column_int64(stmt, 8);
		rows.push_back(row.id);
		if (!seen.emplace(GroupKey(row), groups.size()).second)
			continue;
		CallableGroup group;
		group.id = row.id;
		group.name = row.name;
		group.file_path = row.file_path;
		group.start_row = row.start_row;
		group.start_col = row.start_col;
		groups.push_back(std::move(group));
	}
	if (rc != SQLITE_DONE)
		std::fprintf(stderr,
			     "[module=query, method=resolveCallableGroups] "
			     "step failed: %s\n",
			     sqlite3_errmsg(db));
	sqlite3_finalize(stmt);
	return groups;
}

std::string callableCandidatesJson(const std::vector<CallableGroup> &groups)
{
	std::string out = "[";
	bool first = true;
	for (const auto &group : groups) {
		if (!first)
			out += ',';
		first = false;
		util::JsonWriter el;
		el.beginObject();
		el.key("graph_node_id").value(group.id);
		el.key("name").value(group.name);
		el.key("file_path").value(group.file_path);
		el.key("start_row").value(group.start_row);
		el.key("start_col").value(group.start_col);
		el.endObject();
		out += el.str();
	}
	out += ']';
	return out;
}

} // namespace query
