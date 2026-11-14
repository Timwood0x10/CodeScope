// engine_briefing_sections.cpp — languages / modules / entry-points sections.

#include "engine_briefing_sections.h"

#include <algorithm>
#include <string>
#include <vector>

namespace engine_briefing
{
namespace
{

/// Write a capped list with total/returned/has_more metadata.
/// Each item is written by `write_item`; the caller has already emitted the
/// section key and opened the outer object.
template <typename T, typename WriteItem>
void writeCappedSection(util::JsonWriter &w, const std::vector<T> &items,
			int max_count, const char *source, WriteItem write_item)
{
	const int returned =
		std::min(static_cast<int>(items.size()), max_count);
	w.key("items").beginArray();
	for (int i = 0; i < returned; ++i)
		write_item(w, items[i]);
	w.endArray();
	// The list is fetched whole, so this is a real total, not a page size.
	w.key("total").value(static_cast<int64_t>(items.size()));
	w.key("returned").value(returned);
	w.key("has_more").value(static_cast<int>(items.size()) > max_count);
	w.key("source").value(source);
}

/// Last path component of a parsed module path ("engine/src/store" →
/// "store"). The parser records the directory; the briefing shows both.
std::string moduleName(const std::string &path)
{
	const std::size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void writeLanguages(util::JsonWriter &w, const std::vector<LanguageRow> &langs,
		    int max_count)
{
	w.key("languages");
	w.beginObject();
	writeCappedSection(w, langs, max_count, "fact",
			   [](util::JsonWriter &jw, const LanguageRow &r) {
				   jw.beginObject();
				   jw.key("name").value(r.language);
				   jw.key("symbols").value(r.count);
				   jw.endObject();
			   });
	w.endObject();
}

void writeModules(util::JsonWriter &w, const std::vector<ModuleRow> &mods,
		  int max_count)
{
	w.key("modules");
	w.beginObject();
	writeCappedSection(w, mods, max_count, "fact",
			   [](util::JsonWriter &jw, const ModuleRow &r) {
				   jw.beginObject();
				   jw.key("name").value(moduleName(r.path));
				   jw.key("path").value(r.path);
				   jw.key("symbols").value(r.entity_count);
				   jw.key("role").value("inferred");
				   jw.endObject();
			   });
	w.endObject();
}

void writeEntryPoints(util::JsonWriter &w,
		      const std::vector<EntryPointRow> &eps, int max_count)
{
	w.key("entry_points");
	w.beginObject();
	writeCappedSection(w, eps, max_count, "inferred",
			   [](util::JsonWriter &jw, const EntryPointRow &r) {
				   jw.beginObject();
				   jw.key("name").value(r.name);
				   jw.key("file").value(r.file);
				   jw.key("line").value(r.line);
				   jw.key("source").value("inferred");
				   jw.endObject();
			   });
	w.endObject();
}

} // namespace engine_briefing
