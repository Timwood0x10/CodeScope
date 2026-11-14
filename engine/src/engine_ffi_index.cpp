// engine_ffi_index.cpp — batch indexing and project metadata.
//
// Split out of engine_ffi.cpp (see plan/rules/code_rules.md 1000-line
// rule). engine_index_batch is the worker subprocess entry point that
// parses and inserts a whole file list in one call;
// engine_get_project_info reports what was indexed (license, counts,
// timestamps). Both describe the project on disk rather than querying
// the graph.
// Every function follows the FFI safety contract in engine_ffi.cpp:
// try/catch around the body, null-checked inputs, dupString() result
// the caller frees with engine_free_string().
//
// Content fingerprints are SHA-256, not a 32-bit hash: files.content_hash is
// the leaf of GraphStore::worktreeDigest, so a weak leaf would cap the whole
// chain and let an edit slip past a re-index (development_plan §17.6).

#include "engine_internal.h"
#include "async_knowledge.h"
#include "platform_win.h"
#include "util/json_writer.h"
#include "util/loc_counter.h"
#include "store/store_parse_failure.h"
#include "store/store_identity.h"

#include <cstdio>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Batch Indexing ──────────────────────────────────────────

/// One parsed file waiting to be written.
struct FileBatch {
	std::unique_ptr<ir::TranslationUnit> unit;
	std::string source;
	std::string language;
	std::string file_path;
	FileBatch(std::unique_ptr<ir::TranslationUnit> u, std::string s,
		  std::string l, std::string fp)
		: unit(std::move(u))
		, source(std::move(s))
		, language(std::move(l))
		, file_path(std::move(fp))
	{
	}
};

/// Parse a JSON array of paths. `err` carries the message the caller reports.
static bool parsePathList(const char *json, std::vector<std::string> &paths,
			  std::string &err)
{
	const char *p = json;
	if (!p || !*p) {
		err = "empty file list";
		return false;
	}
	while (*p && *p != '[')
		p++;
	if (!*p) {
		err = "expected [";
		return false;
	}
	p++;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
		       *p == ',')
			p++;
		if (*p == ']')
			break;
		if (*p != '"') {
			err = "expected string";
			return false;
		}
		p++;
		std::string path;
		while (*p && *p != '"') {
			if (*p == '\\') {
				p++;
				switch (*p) {
				case '"':
					path += '"';
					break;
				case '\\':
					path += '\\';
					break;
				case '/':
					path += '/';
					break;
				case 'n':
					path += '\n';
					break;
				case 't':
					path += '\t';
					break;
				case 'r':
					path += '\r';
					break;
				case 'b':
					path += '\b';
					break;
				case 'f':
					path += '\f';
					break;
				case 'u': {
					// Pass-through for \uXXXX: keep the escape
					// text so non-ASCII paths round-trip.
					// Buffer: '\' 'u' + 5 hex + NUL = 8 bytes.
					char unicode_buf[8] = "\\u";
					int i = 1;
					while (*++p && i < 6 &&
					       ((*p >= '0' && *p <= '9') ||
						(*p >= 'a' && *p <= 'f') ||
						(*p >= 'A' && *p <= 'F')))
						unicode_buf[++i] = *p;
					unicode_buf[++i] = '\0';
					path += unicode_buf;
					p--; // loop advances p
					break;
				}
				default:
					path += '\\';
					path += *p;
					break;
				}
			} else {
				path += *p;
			}
			p++;
		}
		if (*p != '"') {
			err = "unterminated string";
			return false;
		}
		p++;
		if (!path.empty())
			paths.push_back(path);
	}
	return true;
}

/// Parse every path in memory; unreadable or unparsable files land in `errors`
/// instead of aborting the batch (one bad file must not lose the rest).
static void parseBatches(EngineContext *ctx,
			 const std::vector<std::string> &paths,
			 std::vector<FileBatch> &batches,
			 std::vector<std::string> &errors)
{
	for (const auto &fp : paths) {
		const char *lang = detectLanguage(fp.c_str());
		if (!lang) {
			errors.push_back(fp + ": unsupported");
			continue;
		}

		std::string source = readFile(fp.c_str());
		if (source.empty()) {
			errors.push_back(fp + ": cannot read");
			continue;
		}

		TSTree *tree = ctx->parser->parse(fp.c_str(), source.c_str(),
						  lang, source.size());
		if (!tree) {
			errors.push_back(fp + ": parse failed");
			continue;
		}

		std::unique_ptr<ir::Translator> translator(
			ir::createTranslator(lang));
		if (!translator) {
			ts_tree_delete(tree);
			errors.push_back(fp + ": no translator");
			continue;
		}

		ir::TranslationUnit *unit =
			translator->translate(tree, source.c_str(), fp.c_str());
		ts_tree_delete(tree);
		if (!unit) {
			errors.push_back(fp + ": translation failed");
			continue;
		}

		batches.push_back(
			FileBatch{ std::unique_ptr<ir::TranslationUnit>(unit),
				   std::move(source), lang, fp });
	}
}

/// Write every parsed file in one transaction.
///
/// Cleanup is a precondition, not a best effort: leftover graph rows plus the
/// inserts below produce duplicate entities. The old code logged the failure
/// and carried on, which is exactly the duplicate accumulation the DELETE
/// exists to prevent.
static bool persistBatches(EngineContext *ctx, uint64_t project_id,
			   std::vector<FileBatch> &batches, int &total_nodes,
			   int &total_edges)
{
	ctx->store->beginTransaction();

	uint64_t start_id = 1;
	{
		// Allocate past BOTH tables: insertEntity is INSERT OR IGNORE
		// (drops on collision) while insertGraphNode does a bare INSERT
		// on graph_nodes.id (PRIMARY KEY). Sourcing only one table lets
		// the other collide and silently lose rows.
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(
			    ctx->store->handle(),
			    "SELECT COALESCE(MAX(t.id),0)+1 FROM (SELECT id "
			    "FROM entity UNION ALL SELECT id FROM "
			    "graph_nodes) t",
			    -1, &stmt, nullptr) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				start_id = static_cast<uint64_t>(
					sqlite3_column_int64(stmt, 0));
			sqlite3_finalize(stmt);
		}
	}

	graph::GraphBuilder builder(project_id, start_id);
	for (auto &b : batches) {
		std::string hash = store::Sha256Hex(b.source);
		auto loc = util::countLoc(b.source);
		ctx->store->upsertFile(project_id, b.file_path.c_str(),
				       b.language.c_str(), hash.c_str(),
				       loc.loc, loc.code_loc, loc.comment_loc,
				       loc.blank_loc);
		// Delete graph-layer data only (relation, graph_edges,
		// graph_nodes, entity, type_ref, type_info, import, route).
		// NOT semantic_records — this batch path re-inserts graph data
		// directly from in-memory `b.unit` and never repopulates
		// semantic_records. buildGraph later uses semantic_records to
		// decide the file rebuild set; wiping it here would make
		// subsequent engine_index_project skip rebuilding this file,
		// leaving stale graph_nodes forever.
		if (!ctx->store->deleteGraphDataByFile(project_id,
						       b.file_path.c_str()))
			return false;

		// No ir_nodes/ir_semantic_edges write — graph_nodes is canonical.
		// FTS/vector writes skipped for single-file index path.

		auto sg = builder.buildSymbolGraph(b.unit.get());
		auto cg = builder.buildCallGraph(b.unit.get());
		for (auto &gn : sg.nodes) {
			ctx->store->insertGraphNode(project_id, gn);
			ctx->store->insertEntity(project_id, gn);
			total_nodes++;
		}
		for (auto &e : sg.edges) {
			ctx->store->insertGraphEdge(project_id, e);
			total_edges++;
		}
		for (auto &e : cg.edges) {
			ctx->store->insertGraphEdge(project_id, e);
			total_edges++;
		}
	}

	ctx->store->commitTransaction();
	return true;
}

char *engine_index_batch(engine_t handle, uint64_t project_id,
			 const char *file_paths_json)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!file_paths_json || !*file_paths_json)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_index_batch] file_paths_json is required\"}");
		if (!ctx || !ctx->store || !ctx->parser)
			return dupString(
				"{\"ok\":false,\"error\":\"not initialized\"}");

		std::vector<std::string> paths;
		std::string parse_err;
		if (!parsePathList(file_paths_json, paths, parse_err))
			return dupString("{\"ok\":false,\"error\":\"" +
					 parse_err + "\"}");
		if (paths.empty())
			return dupString(
				"{\"ok\":false,\"error\":\"empty file list\"}");

		std::vector<FileBatch> batches;
		std::vector<std::string> errors;
		parseBatches(ctx, paths, batches, errors);

		int total_nodes = 0, total_edges = 0;
		if (!persistBatches(ctx, project_id, batches, total_nodes,
				    total_edges)) {
			ctx->store->rollbackTransaction();
			return dupString(util::errorEnvelope(
				"ffi", "engine_index_batch",
				"graph cleanup failed; refusing to insert on "
				"top of stale rows"));
		}

		util::JsonWriter w;
		w.beginObject();
		w.key("ok").value(true);
		w.key("files").value(batches.size() + errors.size());
		w.key("indexed").value(batches.size());
		w.key("nodes").value(total_nodes);
		w.key("edges").value(total_edges);
		w.key("errors").beginArray();
		for (const auto &err : errors)
			w.value(err);
		w.endArray();
		w.endObject();
		return dupString(w.str());
	} catch (const std::exception &e) {
		ctx->store->rollbackTransaction();
		return dupString(util::errorEnvelope(
			"ffi", "engine_index_batch", e.what()));
	} catch (...) {
		ctx->store->rollbackTransaction();
		return dupString(util::errorEnvelope(
			"ffi", "engine_index_batch", "unknown exception"));
	}
}

// ─── Project Metadata ───────────────────────────────────────

static const char *detectLicense(const std::string &content)
{
	if (content.find("Apache License") != std::string::npos ||
	    content.find("Version 2.0, January 2004") != std::string::npos)
		return "Apache-2.0";
	if (content.find("MIT License") != std::string::npos ||
	    content.find("Permission is hereby granted") != std::string::npos)
		return "MIT";
	if (content.find("GNU GENERAL PUBLIC LICENSE") != std::string::npos)
		return content.find("Version 3") != std::string::npos ?
			       "GPL-3.0" :
			       "GPL-2.0";
	if (content.find("BSD") != std::string::npos)
		return "BSD";
	if (content.find("Mozilla Public") != std::string::npos)
		return "MPL-2.0";
	return "Unknown";
}

char *engine_get_project_info(engine_t handle, uint64_t project_id)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!ctx || !ctx->store)
			return dupString("{\"error\":\"not initialized\"}");

		sqlite3 *db = ctx->store->handle();
		std::string name, root;

		{
			sqlite3_stmt *stmt = nullptr;
			const char *sql =
				"SELECT name, root_path FROM projects WHERE id=?";
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					if (sqlite3_column_text(stmt, 0))
						name = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 0));
					if (sqlite3_column_text(stmt, 1))
						root = reinterpret_cast<
							const char *>(
							sqlite3_column_text(
								stmt, 1));
				}
				sqlite3_finalize(stmt);
			}
		}

		// Detect license
		std::string license = "Unknown";
		const char *lfs[] = { "LICENSE",    "LICENSE.txt",
				      "LICENSE.md", "LICENSE-APACHE",
				      "COPYING",    nullptr };
		for (int i = 0; lfs[i]; i++) {
			std::string c = readFile((root + "/" + lfs[i]).c_str());
			if (!c.empty()) {
				license = detectLicense(c);
				break;
			}
		}

		// Primary language
		std::string lang;
		{
			sqlite3_stmt *stmt = nullptr;
			const char *sql =
				"SELECT language,COUNT(*) FROM files WHERE project_id=? "
				"GROUP BY language ORDER BY 2 DESC LIMIT 1";
			if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
			    SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW &&
				    sqlite3_column_text(stmt, 0))
					lang = reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 0));
				sqlite3_finalize(stmt);
			}
		}

		int file_count = 0, dep_count = 0;
		{
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(
				    db,
				    "SELECT COUNT(*) FROM files WHERE project_id=?",
				    -1, &stmt, nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					stmt, 1,
					static_cast<int64_t>(project_id));
				if (sqlite3_step(stmt) == SQLITE_ROW)
					file_count =
						sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
		}

		// Try to parse dep files
		const char *dfs[] = { "go.mod",		  "Cargo.toml",
				      "pyproject.toml",	  "package.json",
				      "requirements.txt", nullptr };
		for (int i = 0; dfs[i]; i++) {
			std::string c = readFile((root + "/" + dfs[i]).c_str());
			if (!c.empty()) {
				int lines = 0;
				for (size_t p = 0;
				     (p = c.find('\n', p)) != std::string::npos;
				     lines++, p++)
					;
				dep_count = lines / 3;
				break;
			}
		}

		util::JsonWriter w;
		w.beginObject();
		w.key("name").value(name);
		w.key("license").value(license);
		w.key("language").value(lang);
		w.key("file_count").value(file_count);
		w.key("dependency_count").value(dep_count);
		w.endObject();
		return dupString(w.str());
	} catch (const std::exception &e) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_info", e.what()));
	} catch (...) {
		return dupString(util::errorEnvelope(
			"ffi", "engine_get_project_info", "unknown exception"));
	}
}

// ─── Parse-failure Maintenance ───────────────────────────────
// parse_failures is written by the indexing paths and drives the fail-fast
// skip. Nothing else exported it, so the table could be written and never read
// or cleared from the CLI — the `codescope parse-failures` and
// `codescope reset-failures` subcommands were documented but had no entry
// point, and store::getParseFailuresJson / store::resetParseFailures were
// unreachable. These two exports are that entry point.

/// Read the project's parse_failures rows as JSON.
/// \param project_id  Project whose rows are read.
/// \param limit       Maximum rows (<= 0 means the store default of 100).
/// \return JSON `{"ok":true,"parse_failures":[...]}`; on failure an
///         `{"ok":false,"error":"...","module":...,"method":...}` envelope.
///         The caller frees it with engine_free_string().
char *engine_get_parse_failures(engine_t handle, uint64_t project_id, int limit)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!ctx || !ctx->store)
			return dupString(
				"{\"ok\":false,\"error\":\"[module=ffi, method=engine_get_parse_failures] engine not initialized\"}");
		const std::string rows = store::getParseFailuresJson(
			ctx, project_id, limit > 0 ? limit : 100);
		return dupString("{\"ok\":true,\"parse_failures\":" + rows +
				 "}");
	} catch (const std::exception &e) {
		return dupString(util::okFalseEnvelope(
			"ffi", "engine_get_parse_failures", e.what()));
	} catch (...) {
		return dupString(util::okFalseEnvelope(
			"ffi", "engine_get_parse_failures",
			"unknown exception"));
	}
}

/// Delete every parse_failures row of the project.
/// \param project_id  Project whose rows are cleared.
/// \return JSON `{"ok":true,"removed":N}` (N may be 0); on failure an
///         `{"ok":false,"error":"..."}` envelope. The caller frees it with
///         engine_free_string().
char *engine_reset_parse_failures(engine_t handle, uint64_t project_id)
{
	EngineContext *ctx = engineInstance(handle);

	try {
		if (!ctx || !ctx->store)
			return dupString(
				"{\"ok\":false,\"error\":\"[module=ffi, method=engine_reset_parse_failures] engine not initialized\"}");
		const int removed = store::resetParseFailures(ctx, project_id);
		if (removed < 0)
			return dupString(
				"{\"ok\":false,\"error\":\"[module=ffi, method=engine_reset_parse_failures] delete failed\"}");
		util::JsonWriter w;
		w.beginObject();
		w.key("ok").value(true);
		w.key("removed").value(removed);
		w.endObject();
		return dupString(w.str());
	} catch (const std::exception &e) {
		return dupString(util::okFalseEnvelope(
			"ffi", "engine_reset_parse_failures", e.what()));
	} catch (...) {
		return dupString(util::okFalseEnvelope(
			"ffi", "engine_reset_parse_failures",
			"unknown exception"));
	}
}
