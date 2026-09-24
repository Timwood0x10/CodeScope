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

#include "engine_internal.h"
#include "async_knowledge.h"
#include "platform_win.h"

#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Batch Indexing ──────────────────────────────────────────

char *engine_index_batch(uint64_t project_id, const char *file_paths_json)
{
	try {
		if (!file_paths_json || !*file_paths_json)
			return dupString(
				"{\"error\":\"[module=ffi, method=engine_index_batch] file_paths_json is required\"}");
		if (!g_store || !g_parser)
			return dupString(
				"{\"ok\":false,\"error\":\"not initialized\"}");

		// Parse JSON array of file paths
		std::vector<std::string> paths;
		{
			const char *p = file_paths_json;
			if (!p || !*p)
				return dupString(
					"{\"ok\":false,\"error\":\"empty file list\"}");
			while (*p && *p != '[')
				p++;
			if (!*p)
				return dupString(
					"{\"ok\":false,\"error\":\"expected [\"}");
			p++;
			while (*p) {
				while (*p == ' ' || *p == '\t' || *p == '\n' ||
				       *p == '\r' || *p == ',')
					p++;
				if (*p == ']')
					break;
				if (*p != '"')
					return dupString(
						"{\"ok\":false,\"error\":\"expected string\"}");
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
							// Simple pass-through for \uXXXX.
							// Buffer layout: '\' 'u' + up to 5 hex
							// digits + NUL = 8 bytes. The loop below
							// lets i reach 6, then writes the NUL at
							// index 7, so the buffer must hold 8.
							char unicode_buf[8] =
								"\\u";
							int i = 1;
							while (*++p && i < 6 &&
							       ((*p >= '0' &&
								 *p <= '9') ||
								(*p >= 'a' &&
								 *p <= 'f') ||
								(*p >= 'A' &&
								 *p <= 'F')))
								unicode_buf[++i] =
									*p;
							unicode_buf[++i] = '\0';
							path += unicode_buf;
							p--; // loop will advance p
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
				if (*p != '"')
					return dupString(
						"{\"ok\":false,\"error\":\"unterminated string\"}");
				p++;
				if (!path.empty())
					paths.push_back(path);
			}
		}
		if (paths.empty())
			return dupString(
				"{\"ok\":false,\"error\":\"empty file list\"}");

		// Phase 1: Parse all files in memory (no DB I/O)
		struct FileBatch {
			std::unique_ptr<ir::TranslationUnit> unit;
			std::string source;
			std::string language;
			std::string file_path;
			FileBatch(std::unique_ptr<ir::TranslationUnit> u,
				  std::string s, std::string l, std::string fp)
				: unit(std::move(u))
				, source(std::move(s))
				, language(std::move(l))
				, file_path(std::move(fp))
			{
			}
		};
		std::vector<FileBatch> batches;
		std::vector<std::string> errors;

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

			TSTree *tree = g_parser->parse(fp.c_str(),
						       source.c_str(), lang,
						       source.size());
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

			ir::TranslationUnit *unit = translator->translate(
				tree, source.c_str(), fp.c_str());
			ts_tree_delete(tree);
			if (!unit) {
				errors.push_back(fp + ": translation failed");
				continue;
			}

			batches.push_back(FileBatch{
				std::unique_ptr<ir::TranslationUnit>(unit),
				std::move(source), lang, fp });
		}

		// Phase 2: Persist in single transaction
		g_store->beginTransaction();

		uint64_t start_id = 1;
		{
			// Allocate past BOTH tables: insertEntity is INSERT OR
			// IGNORE (drops on collision) while insertGraphNode does a
			// bare INSERT on graph_nodes.id (PRIMARY KEY). Sourcing only
			// one table lets the other collide and silently lose rows.
			sqlite3_stmt *stmt = nullptr;
			if (sqlite3_prepare_v2(
				    g_store->handle(),
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
		int total_nodes = 0, total_edges = 0;

		for (auto &b : batches) {
			std::string hash = simpleHash(b.source);
			g_store->upsertFile(project_id, b.file_path.c_str(),
					    b.language.c_str(), hash.c_str());
			// Delete graph-layer data only (relation, graph_edges,
			// graph_nodes, entity, type_ref, type_info, import, route).
			// NOT semantic_records — this batch path re-inserts graph
			// data directly from in-memory `b.unit` and never repopulates
			// semantic_records. buildGraph later uses semantic_records to
			// decide the file rebuild set; wiping it here would make
			// subsequent engine_index_project skip rebuilding this file,
			// leaving stale graph_nodes forever.
			g_store->deleteGraphDataByFile(project_id,
						       b.file_path.c_str());

			// No ir_nodes/ir_semantic_edges write — graph_nodes is canonical.
			// FTS/vector writes skipped for single-file index path.

			auto sg = builder.buildSymbolGraph(b.unit.get());
			auto cg = builder.buildCallGraph(b.unit.get());
			for (auto &gn : sg.nodes) {
				g_store->insertGraphNode(project_id, gn);
				g_store->insertEntity(project_id, gn);
				total_nodes++;
			}
			for (auto &e : sg.edges) {
				g_store->insertGraphEdge(project_id, e);
				total_edges++;
			}
			for (auto &e : cg.edges) {
				g_store->insertGraphEdge(project_id, e);
				total_edges++;
			}

			// ComplexityAnalyzer removed (Phase 0)
			for (auto &gn : sg.nodes)
				if (gn.type == graph::NodeType::Function ||
				    gn.type == graph::NodeType::Method)
					for (auto *in : b.unit->all_nodes)
						if (in->id == gn.ir_node_id) {
							break;
						}
		}

		g_store->commitTransaction();

		std::ostringstream r;
		r << "{\"ok\":true,\"files\":"
		  << (batches.size() + errors.size())
		  << ",\"indexed\":" << batches.size()
		  << ",\"nodes\":" << total_nodes
		  << ",\"edges\":" << total_edges << ",\"errors\":[";
		for (size_t i = 0; i < errors.size(); i++) {
			if (i > 0)
				r << ",";
			r << "\"" << jsonEscape(errors[i]) << "\"";
		}
		r << "]}";
		return dupString(r.str());
	} catch (const std::exception &e) {
		g_store->rollbackTransaction();
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_index_batch] ") +
			e.what() + "\"}");
	} catch (...) {
		g_store->rollbackTransaction();
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_index_batch] unknown exception\"}");
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

char *engine_get_project_info(uint64_t project_id)
{
	try {
		auto _store_guard = waitForKnowledgeBuilder();
		if (!g_store)
			return dupString("{\"error\":\"not initialized\"}");

		sqlite3 *db = g_store->handle();
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

		std::ostringstream j;
		j << "{\"name\":\"" << jsonEscape(name) << "\",\"license\":\""
		  << jsonEscape(license) << "\",\"language\":\""
		  << jsonEscape(lang) << "\",\"file_count\":" << file_count
		  << ",\"dependency_count\":" << dep_count << "}";
		return dupString(j.str());
	} catch (const std::exception &e) {
		return dupString(
			std::string(
				"{\"error\":\"[module=ffi, method=engine_get_project_info] ") +
			e.what() + "\"}");
	} catch (...) {
		return dupString(
			"{\"error\":\"[module=ffi, method=engine_get_project_info] unknown exception\"}");
	}
}
