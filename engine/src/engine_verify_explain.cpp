// engine_verify_explain.cpp — the module Knowledge Card.
//
// Split out of engine_verify_ffi.cpp (see plan/rules/code_rules.md
// 1000-line rule). engine_explain_module is the biggest single export in
// the verify surface: it assembles summary, entities, capabilities,
// contracts, findings and integrity score into one JSON card. Kept
// together because every section is a step of the same card build and
// they share the same verifier-registry bootstrapping.

#include "engine_internal.h"
#include "async_knowledge.h"
#include "platform_win.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "verify/architecture_verifier.h"
#include "verify/architecture_drift.h"
#include "verify/capability_drift.h"
#include "verify/capability_verifier.h"
#include "verify/claim.h"
#include "verify/claim_parser.h"
#include "verify/contract_verifier.h"
#include "verify/documentation_drift.h"
#include "verify/function_implements_verifier.h"
#include "verify/registry.h"
#include "verify/dead_code_inspector.h"
#include "verify/ffi_internal.h"

// ─── Named Constants (local to this translation unit) ──────────
// Duplicated from engine_verify_ffi.cpp rather than shared: they are
// TU-local tuning knobs, and a shared header would make it easy to change
// one export's scoring while silently changing another's.

// Maximum number of entity sample rows returned by engine_explain_module.
static constexpr int kEntitySampleLimit = 10;
/// Maximum number of cross-module dependency edges to return per direction.
static constexpr int kCrossModuleEdgeLimit = 20;

// Integrity score parameters for engine_explain_module.
static constexpr int kIntegrityMax = 100;
static constexpr int kIntegritySev2Penalty = 10;
static constexpr int kIntegritySev1Penalty = 5;

// engine_explain_module — return a Knowledge Card for a named module.
//
// Looks up the module by name (case-insensitive) in the modules table. If no
// row exists, falls back to deriving module info from file paths so the tool
// works on freshly-indexed projects where the modules table has not yet been
// populated.
//
// Output JSON (Knowledge Card):
//   {"module":"engine","summary":"...","entities":{"count":N,"sample":[...]},
//    "capabilities":[...],"contracts":[...],"findings":[...],
//    "integrity":92}
// On error: {"error":"module not found","module":"<name>"}
//
// MEMORY: caller MUST free the returned char* via engine_free_string().
// THREAD SAFETY: single-threaded (GraphStore writer invariant).
extern "C" char *engine_explain_module(uint64_t project_id,
				       const char *module_name)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString(
				"{\"error\":\"not initialized "
				"[module=ffi, method=engine_explain_module]\"}");
		if (!module_name || !*module_name)
			return dupString(
				"{\"error\":\"module_name is empty "
				"[module=ffi, method=engine_explain_module]\"}");

		std::string name(module_name);
		std::string name_lower;
		name_lower.reserve(name.size());
		for (char c : name)
			name_lower += static_cast<char>(
				std::tolower(static_cast<unsigned char>(c)));

		sqlite3 *db = g_store->handle();
		if (!db)
			return dupString(
				"{\"error\":\"db not open "
				"[module=ffi, method=engine_explain_module]\"}");

		// Resolve module row (case-insensitive name match).
		// No project_id filter: module names are globally unique in
		// both serial (single project) and parallel (merged) products,
		// and the MCP layer's restored project_id may differ from the
		// owning module's project_id in parallel products.
		std::string summary;
		bool found = false;
		{
			const char *sql = "SELECT name FROM modules "
					  "WHERE LOWER(name)=? "
					  "LIMIT 1";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, name_lower.c_str(),
						  -1, SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					found = true;
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					if (n)
						summary = "Module: " +
							  std::string(n);
				}
				sqlite3_finalize(stmt);
			}
		}

		// Fallback: derive module existence from file paths so the tool works
		// even when the modules table is empty (e.g. freshly indexed project).
		// Use "%/<name>/%" so paths with a leading "./" or a parent directory
		// still match. The slashes prevent partial segment matches (e.g. a
		// query for "engine" won't match "./my_engine/foo").
		if (!found) {
			// Build the LIKE pattern. The fallback targets files under
			// `name`, so an absolute path (leading '/') must not gain a
			// second slash: "%/" + "/Users/..." would produce
			// "%//Users/..." which never matches a single-slash path.
			// Relative module names keep the leading "/" to avoid
			// partial-segment matches ("engine" vs "./my_engine").
			std::string like = name[0] == '/' ? "%" + name + "/%" :
							    "%/" + name + "/%";
			const char *sql = "SELECT COUNT(*) FROM files "
					  "WHERE path LIKE ?";
			sqlite3_stmt *stmt = nullptr;
			int count = 0;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, like.c_str(), -1,
						  SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW)
					count = sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
			if (count == 0) {
				std::ostringstream j;
				j << "{\"error\":\"module not found\",\"module\":\""
				  << jsonEscape(name) << "\"}";
				return dupString(j.str());
			}
			found = true;
			summary = "Module derived from " +
				  std::to_string(count) + " files under " +
				  name + "/";
		}

		std::ostringstream json;
		json << "{\"module\":\"" << jsonEscape(name) << "\","
		     << "\"summary\":\"" << jsonEscape(summary) << "\",";

		// Entities: count + sample (up to 10) for this module's files.
		// Use the same "%/<name>/%" pattern as the fallback check above so
		// paths with a leading "./" match consistently.
		{
			std::string like = "%/" + name + "/%";
			// v0.2.5: read from the canonical `entity` table (the legacy
			// graph_nodes table is empty in the canonical schema, so this
			// previously always returned zero entities). entity.id
			// preserves the legacy graph node identity.
			std::string sql_str =
				"SELECT name, kind, file_path FROM entity "
				"WHERE file_path LIKE ? "
				"ORDER BY id LIMIT " +
				std::to_string(kEntitySampleLimit);
			sqlite3_stmt *stmt = nullptr;
			int total = 0;
			// Count first
			const char *csql = "SELECT COUNT(*) FROM entity "
					   "WHERE file_path LIKE ?";
			if (sqlite3_prepare_v2(db, csql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, like.c_str(), -1,
						  SQLITE_STATIC);
				if (sqlite3_step(stmt) == SQLITE_ROW)
					total = sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
			json << "\"entities\":{\"count\":" << total
			     << ",\"sample\":[";
			if (sqlite3_prepare_v2(db, sql_str.c_str(), -1, &stmt,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, like.c_str(), -1,
						  SQLITE_STATIC);
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					int kind = sqlite3_column_int(stmt, 1);
					const char *fp =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2));
					json << "{\"name\":\""
					     << jsonEscape(n ? n : "") << "\","
					     << "\"kind\":" << kind << ","
					     << "\"file_path\":\""
					     << jsonEscape(fp ? fp : "")
					     << "\"}";
				}
				sqlite3_finalize(stmt);
			}
			json << "]},";
		}

		// Capabilities
		{
			json << "\"capabilities\":[";
			const char *sql =
				"SELECT id, name, summary FROM capability "
				"ORDER BY id";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					int64_t id =
						sqlite3_column_int64(stmt, 0);
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					const char *s =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2));
					json << "{\"id\":" << id << ","
					     << "\"name\":\""
					     << jsonEscape(n ? n : "") << "\","
					     << "\"summary\":\""
					     << jsonEscape(s ? s : "") << "\"}";
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
		}

		// Contracts
		{
			json << "\"contracts\":[";
			const char *sql =
				"SELECT id, name, origin, claim_text FROM contract "
				"ORDER BY id";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					int64_t id =
						sqlite3_column_int64(stmt, 0);
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					const char *o =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 2));
					const char *c =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 3));
					json << "{\"id\":" << id << ","
					     << "\"name\":\""
					     << jsonEscape(n ? n : "") << "\","
					     << "\"origin\":\""
					     << jsonEscape(o ? o : "") << "\","
					     << "\"claim_text\":\""
					     << jsonEscape(c ? c : "") << "\"}";
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
		}

		// Findings + integrity score
		{
			json << "\"findings\":[";
			const char *sql =
				"SELECT id, rule, severity, description, confidence "
				"FROM finding ORDER BY id";
			sqlite3_stmt *stmt = nullptr;
			int sev2 = 0, sev1 = 0;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					int64_t id =
						sqlite3_column_int64(stmt, 0);
					const char *r =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 1));
					int sev = sqlite3_column_int(stmt, 2);
					const char *d =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 3));
					double conf =
						sqlite3_column_double(stmt, 4);
					json << "{\"id\":" << id << ","
					     << "\"rule\":\""
					     << jsonEscape(r ? r : "") << "\","
					     << "\"severity\":" << sev << ","
					     << "\"description\":\""
					     << jsonEscape(d ? d : "") << "\","
					     << "\"confidence\":" << conf
					     << "}";
					if (sev == 2)
						sev2++;
					else if (sev == 1)
						sev1++;
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
			int integrity = kIntegrityMax -
					kIntegritySev2Penalty * sev2 -
					kIntegritySev1Penalty * sev1;
			if (integrity < 0)
				integrity = 0;
			if (integrity > kIntegrityMax)
				integrity = kIntegrityMax;
			json << "\"integrity\":" << integrity << ",";
		}

		// Cross-module dependencies from the pre-computed module_edge table.
		// Populated by the async knowledge builder after indexing. When the
		// table is empty (e.g. async build not yet complete), both arrays are
		// empty — the card still returns successfully.
		json << "\"cross_module\":{";

		// depends_on: modules that this module calls (outgoing edges).
		{
			json << "\"depends_on\":[";
			const char *sql =
				"SELECT tgt_module, edge_count "
				"FROM module_edge "
				"WHERE project_id=? AND src_module LIKE ? "
				"ORDER BY edge_count DESC LIMIT ?";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				std::string like = "%/" + name + "/%";
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(stmt, 2, like.c_str(), -1,
						  SQLITE_STATIC);
				sqlite3_bind_int(stmt, 3,
						 kCrossModuleEdgeLimit);
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					const char *m =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					int64_t ec =
						sqlite3_column_int64(stmt, 1);
					json << "{\"module\":\""
					     << jsonEscape(m ? m : "") << "\""
					     << ",\"edge_count\":" << ec << "}";
				}
				sqlite3_finalize(stmt);
			}
			json << "],";
		}

		// depended_by: modules that call this module (incoming edges).
		{
			json << "\"depended_by\":[";
			const char *sql =
				"SELECT src_module, edge_count "
				"FROM module_edge "
				"WHERE project_id=? AND tgt_module LIKE ? "
				"ORDER BY edge_count DESC LIMIT ?";
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				std::string like = "%/" + name + "/%";
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				sqlite3_bind_text(stmt, 2, like.c_str(), -1,
						  SQLITE_STATIC);
				sqlite3_bind_int(stmt, 3,
						 kCrossModuleEdgeLimit);
				bool first = true;
				while (sqlite3_step(stmt) == SQLITE_ROW) {
					if (!first)
						json << ",";
					first = false;
					const char *m =
						reinterpret_cast<const char *>(
							sqlite3_column_text(
								stmt, 0));
					int64_t ec =
						sqlite3_column_int64(stmt, 1);
					json << "{\"module\":\""
					     << jsonEscape(m ? m : "") << "\""
					     << ",\"edge_count\":" << ec << "}";
				}
				sqlite3_finalize(stmt);
			}
			json << "]";
		}

		json << "}}";
		return dupString(json.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_explain_module] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_explain_module] unknown exception\"}");
	}
}
