// engine_evidence_ffi.cpp — Evidence Builder FFI exports (EB4).
//
// Exposes evidence::EvidenceBuilder to the Rust MCP server via a
// single extern "C" entry point:
//
//   char *engine_build_evidence(uint64_t project_id,
//                                const char *category_filter);
//
// The function loads rule files from the directory pointed to by
// CODESCOPE_RULES_DIR (falling back to "engine/src/evidence/rules"
// relative to CWD), runs all rules (or one category's rules when
// `category_filter` is non-empty), and returns a JSON array of
// Evidence objects. The caller MUST release the returned pointer via
// engine_free_string().
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
#include "evidence/evidence_builder.h"
#include "platform_win.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// ─── Local helpers ──────────────────────────────────────────────

namespace
{

// JSON-escape a string for inclusion in a JSON string literal.
// Mirrors the jsonEscape helper in engine_internal.h but kept
// local to avoid pulling that header's full set of includes.
std::string escapeJson(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x",
					      static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out;
}

// Serialize one EvidenceItem to a JSON object string.
std::string serializeItem(const evidence::EvidenceItem &item)
{
	std::ostringstream ss;
	ss << "{\"fact_id\":" << item.fact_id << ",\"category\":\""
	   << escapeJson(item.category) << "\""
	   << ",\"primitive\":\"" << escapeJson(item.primitive) << "\""
	   << ",\"kind\":\"" << escapeJson(item.kind) << "\""
	   << ",\"symbol\":\"" << escapeJson(item.symbol) << "\""
	   << ",\"file\":\"" << escapeJson(item.file) << "\""
	   << ",\"line\":" << item.line << ",\"snippet\":\""
	   << escapeJson(item.snippet) << "\""
	   << "}";
	return ss.str();
}

// Serialize one Evidence to a JSON object string. `items` is always
// emitted as an array (empty for Count combine).
std::string serializeEvidence(const evidence::Evidence &ev)
{
	std::ostringstream ss;
	ss << "{\"category\":\"" << escapeJson(ev.category) << "\""
	   << ",\"title\":\"" << escapeJson(ev.title) << "\""
	   << ",\"confidence\":" << ev.confidence << ",\"items\":[";
	for (size_t i = 0; i < ev.items.size(); ++i) {
		if (i)
			ss << ",";
		ss << serializeItem(ev.items[i]);
	}
	ss << "]}";
	return ss.str();
}

} // namespace

// ─── FFI entry point ────────────────────────────────────────────

// Returns JSON array of evidence for a project. Optionally filter by
// category. Caller must free the returned string via
// engine_free_string. Path: rules_dir defaults to
// "engine/src/evidence/rules" relative to CWD, or override via
// CODESCOPE_RULES_DIR env var.
char *engine_build_evidence(uint64_t project_id, const char *category_filter)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");

	const char *env_dir = std::getenv("CODESCOPE_RULES_DIR");
	std::string rules_dir =
		(env_dir && *env_dir) ? env_dir : "engine/src/evidence/rules";

	evidence::EvidenceBuilder builder(g_store.get());
	builder.loadRules(rules_dir);

	std::vector<evidence::Evidence> evidences;
	if (category_filter && *category_filter) {
		evidences =
			builder.buildByCategory(project_id, category_filter);
	} else {
		evidences = builder.buildAll(project_id);
	}

	std::string json = "[";
	for (size_t i = 0; i < evidences.size(); ++i) {
		if (i)
			json += ",";
		json += serializeEvidence(evidences[i]);
	}
	json += "]";
	return dupString(json);
}
