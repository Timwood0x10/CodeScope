#ifndef CODESCOPE_ENGINE_BRIEFING_SECTIONS_H
#define CODESCOPE_ENGINE_BRIEFING_SECTIONS_H

#include "util/json_writer.h"

#include <cstdint>
#include <string>
#include <vector>

/// Structural sections of the project briefing card.
///
/// Split out of engine_briefing_ffi.cpp: the card is the largest single
/// response in the engine and was at the §1.1 warning line, while these three
/// writers only format rows the parent already fetched. Moving them keeps the
/// card's queries and its byte budget in one file without growing it.
namespace engine_briefing
{

/// Per-list display caps (§18.7.0). The lists are fetched whole, so `total` in
/// each section is a real count and `has_more` means truncation.
constexpr int kMaxLanguages = 8;
constexpr int kMaxModules = 12;
constexpr int kMaxEntryPoints = 8;

struct LanguageRow {
	std::string language;
	int64_t count = 0;
};

struct ModuleRow {
	std::string path;
	int64_t entity_count = 0;
};

struct EntryPointRow {
	std::string name;
	std::string file;
	int64_t line = 0;
};

/// Write the languages section (name + symbol count).
void writeLanguages(util::JsonWriter &w, const std::vector<LanguageRow> &langs,
		    int max_count);

/// Write the modules section. Role stays "inferred": it is derived from the
/// parsed path, not declared by the project.
void writeModules(util::JsonWriter &w, const std::vector<ModuleRow> &mods,
		  int max_count);

/// Write the entry-point candidates. Always source=inferred: the name
/// whitelist can only propose candidates, never a complete entry set.
void writeEntryPoints(util::JsonWriter &w,
		      const std::vector<EntryPointRow> &eps, int max_count);

} // namespace engine_briefing

#endif // CODESCOPE_ENGINE_BRIEFING_SECTIONS_H
