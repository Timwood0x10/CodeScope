// engine_evidence_ffi.cpp — Evidence Builder FFI exports (EB4).
//
// Exposes evidence::EvidenceBuilder to the Rust MCP server via a
// single extern "C" entry point:
//
//   char *engine_build_evidence(uint64_t project_id,
//                                const char *category_filter);
//
// The function loads rule files from the directory returned by
// evidence::resolveRulesDir() ($CODESCOPE_RULES_DIR, then the build-
// time default, then the in-tree relative path), runs all rules (or
// one category's rules when `category_filter` is non-empty), and
// returns a JSON array of Evidence objects. The caller MUST release
// the returned pointer via engine_free_string().
//
// Output shape (JSON array of objects):
//   [
//     {
//       "category": "sync",
//       "title": "1 function(s) lock mutex without defer Unlock",
//       "confidence": 1.0,
//       "items": [
//         { "fact_id": 12, "category": "sync", "primitive": "mutex",
//           "kind": "lock", "symbol": "m.Lock",
//           "file": "/src/sync.go", "line": 5,
//           "snippet": "m.Lock (/src/sync.go)" }
//       ]
//     }
//   ]
//
// All errors return a JSON object with an "error" field instead of
// crashing. Null `g_store` returns
//   {"error":"engine not initialized"}.

#include "engine_internal.h"
#include "async_knowledge.h"
#include "evidence/evidence_builder.h"
#include "platform_win.h"
#include "util/json_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ─── Local helpers ──────────────────────────────────────────────

namespace
{

// Serialize one EvidenceItem into the object currently open on `w`.
void writeItem(util::JsonWriter &w, const evidence::EvidenceItem &item)
{
	w.beginObject();
	w.key("fact_id").value(item.fact_id);
	w.key("category").value(item.category);
	w.key("primitive").value(item.primitive);
	w.key("kind").value(item.kind);
	w.key("symbol").value(item.symbol);
	w.key("file").value(item.file);
	w.key("line").value(item.line);
	w.key("snippet").value(item.snippet);
	w.endObject();
}

// Serialize one Evidence as an object; `items` is always emitted as an
// array (empty for the Count combine mode).
void writeEvidence(util::JsonWriter &w, const evidence::Evidence &ev)
{
	w.beginObject();
	w.key("category").value(ev.category);
	w.key("title").value(ev.title);
	w.key("confidence").value(ev.confidence);
	w.key("items").beginArray();
	for (const auto &item : ev.items)
		writeItem(w, item);
	w.endArray();
	w.endObject();
}

} // namespace

// ─── FFI entry point ────────────────────────────────────────────

// Returns JSON array of evidence for a project. Optionally filter by
// category. Caller must free the returned string via
// engine_free_string. Rule files are located by
// evidence::resolveRulesDir() so the result does not depend on the
// process working directory.
char *engine_build_evidence(uint64_t project_id, const char *category_filter)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString(
				"{\"error\":\"engine not initialized\"}");

		std::string rules_dir = evidence::resolveRulesDir();
		if (rules_dir.empty())
			return dupString("[]");

		evidence::EvidenceBuilder builder(g_store.get());
		builder.loadRules(rules_dir);

		std::vector<evidence::Evidence> evidences;
		if (category_filter && *category_filter) {
			evidences = builder.buildByCategory(project_id,
							    category_filter);
		} else {
			evidences = builder.buildAll(project_id);
		}

		util::JsonWriter w;
		w.beginArray();
		for (const auto &ev : evidences)
			writeEvidence(w, ev);
		w.endArray();
		return dupString(w.str());
	} catch (const std::exception &e) {
		return dupString(std::string("{\"error\":\"[module=ffi, "
					     "method=engine_build_evidence] ") +
				 jsonEscape(e.what()) + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, "
			"method=engine_build_evidence] unknown exception\"}");
	}
}
