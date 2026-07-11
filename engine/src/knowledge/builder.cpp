#include "builder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace knowledge
{

// ─── Constants ────────────────────────────────────────────────────

// Function-like node kinds (graph::NodeType values stored in
// graph_nodes.node_type): Function=0, Method=1. See graph/graph_types.h.
static constexpr int kKindFunction = 0;
static constexpr int kKindMethod = 1;

// Maximum length of a capability summary (truncated from README line).
static constexpr size_t kMaxSummaryLen = 200;

// Characters of context to append after a contract keyword phrase.
static constexpr size_t kContractContextLen = 60;

// Module + method tag for stderr logging per code_rules.md.
static constexpr const char *kLogPrefix = "[module=knowledge, method=build]";

// ─── String Helpers ──────────────────────────────────────────────

/// Convert a string to lowercase (ASCII only).
static std::string toLower(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
		out.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(c))));
	return out;
}

/// Trim leading and trailing whitespace from a string.
static std::string trim(const std::string &s)
{
	size_t start = 0;
	while (start < s.size() &&
	       std::isspace(static_cast<unsigned char>(s[start])))
		start++;
	size_t end = s.size();
	while (end > start &&
	       std::isspace(static_cast<unsigned char>(s[end - 1])))
		end--;
	return s.substr(start, end - start);
}

/// Truncate a string to maxLen characters, appending "..." if truncated.
static std::string truncate(const std::string &s, size_t maxLen)
{
	if (s.size() <= maxLen)
		return s;
	if (maxLen <= 3)
		return s.substr(0, maxLen);
	return s.substr(0, maxLen - 3) + "...";
}

/// Read the full content of a file from disk.
/// Returns empty string on failure (logged to stderr).
static std::string readFileContent(const std::string &path)
{
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs.is_open()) {
		// File read failure is a soft error — skip this file.
		return {};
	}
	std::ostringstream oss;
	oss << ifs.rdbuf();
	return oss.str();
}

/// Split text into lines (handling \n and \r\n).
static std::vector<std::string> splitLines(const std::string &text)
{
	std::vector<std::string> lines;
	std::string current;
	for (char c : text) {
		if (c == '\n') {
			lines.push_back(current);
			current.clear();
		} else if (c != '\r') {
			current.push_back(c);
		}
	}
	if (!current.empty())
		lines.push_back(current);
	return lines;
}

// ─── Capability Rules ─────────────────────────────────────────────

/// A capability rule maps a set of keyword variants to a PascalCase
/// capability name. If any keyword is found in the README text, the
/// capability is inserted (once per rule, not once per keyword).
struct CapRule {
	const char *name;
	std::vector<std::string> keywords;
};

/// Get the list of capability keyword rules. Keywords are matched
/// case-insensitively against README content.
static const std::vector<CapRule> &capabilityRules()
{
	static const std::vector<CapRule> rules = {
		{ "IncrementalIndex",
		  { "incremental indexing", "incremental index",
		    "incremental" } },
		{ "CallGraph", { "call graph", "callgraph", "call-graph" } },
		{ "FullTextSearch",
		  { "full-text search", "full text search", "fts" } },
		{ "CrossFileResolution",
		  { "cross-file resolution", "cross-file", "cross file" } },
		{ "Plugin",
		  { "plugin system", "plugin architecture", "plugin" } },
		{ "EmbeddingSearch",
		  { "semantic search", "vector search", "embedding" } },
	};
	return rules;
}

// ─── Contract Rules ───────────────────────────────────────────────

/// A contract rule maps keyword variants to a PascalCase contract name.
/// The first matching keyword in the README determines the claim_text.
struct ContractRule {
	const char *name;
	std::vector<std::string> keywords;
};

/// Get the list of contract keyword rules for README scanning.
static const std::vector<ContractRule> &contractRules()
{
	static const std::vector<ContractRule> rules = {
		{ "ThreadSafe", { "thread-safe", "thread safe" } },
		{ "MemorySafe", { "memory-safe", "memory safe" } },
		{ "ZeroCopy", { "zero-copy", "zero copy" } },
		{ "LockFree", { "lockfree", "lock-free" } },
	};
	return rules;
}

// ─── Entry Function Names ────────────────────────────────────────

/// Get the list of entry-function names to look for in the entity table.
static const std::vector<std::string> &entryFunctionNames()
{
	static const std::vector<std::string> names = {
		"main", "run", "init", "setup", "start", "serve",
	};
	return names;
}

// ─── Constructor ─────────────────────────────────────────────────

KnowledgeBuilder::KnowledgeBuilder(store::GraphStore *store,
				   uint64_t project_id)
	: store_(store)
	, project_id_(project_id)
{
}

// ─── Build Entry Point ───────────────────────────────────────────

bool KnowledgeBuilder::build()
{
	if (!store_) {
		fprintf(stderr, "KnowledgeBuilder: null store %s\n",
			kLogPrefix);
		return false;
	}

	// Step 1: Clear existing knowledge rows (idempotent rebuild).
	if (!store_->clearProjectKnowledge(project_id_)) {
		fprintf(stderr,
			"KnowledgeBuilder: clearProjectKnowledge failed: %s "
			"%s\n",
			store_->error().c_str(), kLogPrefix);
		return false;
	}

	// Steps 2-5: Derive knowledge from facts + documents.
	// Each step returns false only on DB errors; file read failures
	// are logged but do not abort the build.
	bool ok = true;
	if (!scanReadmeCapabilities())
		ok = false;
	if (!scanReadmeContracts())
		ok = false;
	if (!findEntryFunctions())
		ok = false;
	if (!scanTodoFixme())
		ok = false;

	if (!ok) {
		fprintf(stderr,
			"KnowledgeBuilder: one or more steps had DB errors "
			"%s\n",
			kLogPrefix);
	}
	return ok;
}

// ─── Step 2: README Capability Scan ──────────────────────────────

/// Query the files table for README files belonging to this project.
/// Also scans the project root filesystem directly for README.md / README
/// because the indexer only processes source files (.cpp/.rs/etc.), not
/// Markdown documents — so README is typically absent from the files table.
/// Returns a deduplicated vector of absolute file paths.
static std::vector<std::string> queryReadmeFiles(store::GraphStore *store,
						 uint64_t project_id)
{
	std::vector<std::string> paths;
	std::unordered_set<std::string> seen;

	// Source 1: files table (covers test fixtures that manually insert
	// README rows, and future indexer versions that may index .md files).
	const char *sql = "SELECT path FROM files "
			  "WHERE project_id=? AND LOWER(path) LIKE '%readme%'";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"KnowledgeBuilder: prepare readme query failed: %s "
			"%s\n",
			sqlite3_errmsg(store->handle()), kLogPrefix);
		return paths;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		if (p && seen.insert(p).second)
			paths.emplace_back(p);
	}
	sqlite3_finalize(stmt);

	// Source 2: project root filesystem. The indexer skips .md files, so
	// we walk the project root for README.md / README directly. Limit to
	// the top 2 directory levels to bound the walk on large monorepos.
	const char *root_sql = "SELECT root_path FROM projects WHERE id=?";
	sqlite3_stmt *root_stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), root_sql, -1, &root_stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"KnowledgeBuilder: prepare root_path query failed: %s "
			"%s\n",
			sqlite3_errmsg(store->handle()), kLogPrefix);
		return paths;
	}
	sqlite3_bind_int64(root_stmt, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(root_stmt) == SQLITE_ROW) {
		const char *r = reinterpret_cast<const char *>(
			sqlite3_column_text(root_stmt, 0));
		if (r) {
			std::filesystem::path root(r);
			// Scan top-level root + one level deep. Deeper nesting
			// (e.g. docs/README.md) is also covered by max_depth=2.
			std::error_code ec;
			for (auto it = std::filesystem::
				     recursive_directory_iterator(
					     root,
					     std::filesystem::directory_options::
						     skip_permission_denied,
					     ec);
			     it !=
			     std::filesystem::recursive_directory_iterator();
			     it.increment(ec)) {
				if (ec)
					continue;
				if (it.depth() > 2)
					it.disable_recursion_pending();
				if (!it->is_regular_file())
					continue;
				std::string fname =
					it->path().filename().string();
				std::string lower;
				lower.reserve(fname.size());
				for (char c : fname)
					lower.push_back(static_cast<
							char>(std::tolower(
						static_cast<unsigned char>(c))));
				if (lower == "readme.md" || lower == "readme") {
					std::string p = it->path().string();
					if (seen.insert(p).second)
						paths.emplace_back(
							std::move(p));
				}
			}
		}
	}
	sqlite3_finalize(root_stmt);
	return paths;
}

bool KnowledgeBuilder::scanReadmeCapabilities()
{
	auto readmePaths = queryReadmeFiles(store_, project_id_);
	if (readmePaths.empty())
		return true; // No READMEs — not an error.

	bool ok = true;
	for (const auto &path : readmePaths) {
		std::string content = readFileContent(path);
		if (content.empty()) {
			fprintf(stderr,
				"KnowledgeBuilder: could not read README %s "
				"%s\n",
				path.c_str(), kLogPrefix);
			continue;
		}

		auto lines = splitLines(content);
		std::string lowerContent = toLower(content);

		// For each capability rule, check if any keyword appears.
		// Insert at most one capability per rule per README.
		for (const auto &rule : capabilityRules()) {
			bool found = false;
			for (const auto &kw : rule.keywords) {
				if (lowerContent.find(kw) !=
				    std::string::npos) {
					found = true;
					break;
				}
			}
			if (!found)
				continue;

			// Build summary from the first line containing any
			// of this rule's keywords.
			std::string summary;
			for (const auto &line : lines) {
				std::string lowerLine = toLower(line);
				bool lineMatches = false;
				for (const auto &kw : rule.keywords) {
					if (lowerLine.find(kw) !=
					    std::string::npos) {
						lineMatches = true;
						break;
					}
				}
				if (lineMatches) {
					summary = truncate(trim(line),
							   kMaxSummaryLen);
					break;
				}
			}
			if (summary.empty())
				summary = rule.name;

			if (!store_->insertCapability(project_id_, rule.name,
						      summary, "readme",
						      path)) {
				fprintf(stderr,
					"KnowledgeBuilder: insertCapability "
					"failed for %s: %s %s\n",
					rule.name, store_->error().c_str(),
					kLogPrefix);
				ok = false;
			}
		}
	}
	return ok;
}

// ─── Step 3: README Contract Scan ────────────────────────────────

bool KnowledgeBuilder::scanReadmeContracts()
{
	auto readmePaths = queryReadmeFiles(store_, project_id_);
	if (readmePaths.empty())
		return true;

	bool ok = true;
	for (const auto &path : readmePaths) {
		std::string content = readFileContent(path);
		if (content.empty())
			continue;

		auto lines = splitLines(content);

		// Track which contracts have been inserted from this README
		// to avoid duplicates (one per contract name per README).
		std::unordered_set<std::string> inserted;

		for (int lineNum = 0; lineNum < static_cast<int>(lines.size());
		     lineNum++) {
			const std::string &line = lines[lineNum];
			std::string lowerLine = toLower(line);

			for (const auto &rule : contractRules()) {
				if (inserted.count(rule.name))
					continue;

				for (const auto &kw : rule.keywords) {
					size_t pos = lowerLine.find(kw);
					if (pos == std::string::npos)
						continue;

					// Build claim_text: the matching phrase
					// plus up to kContractContextLen chars
					// of context after it.
					size_t phraseEnd = pos + kw.size();
					size_t ctxEnd = std::min(
						phraseEnd + kContractContextLen,
						line.size());
					std::string claimText =
						line.substr(pos, ctxEnd - pos);
					claimText = trim(claimText);

					if (!store_->insertContract(
						    project_id_, rule.name,
						    "readme", claimText, path,
						    lineNum + 1)) {
						fprintf(stderr,
							"KnowledgeBuilder: "
							"insertContract failed "
							"for %s: %s %s\n",
							rule.name,
							store_->error().c_str(),
							kLogPrefix);
						ok = false;
					}
					inserted.insert(rule.name);
					break;
				}
			}
		}
	}
	return ok;
}

// ─── Step 4: Entry Function Heuristic ────────────────────────────

bool KnowledgeBuilder::findEntryFunctions()
{
	const auto &names = entryFunctionNames();

	// Query graph_nodes (production source of truth) instead of entity
	// because the bulk buildGraph SQL path bypasses the dual-write. The
	// node_type column matches graph::NodeType values (Function=0,
	// Method=1). See graph/graph_types.h.
	std::string sql =
		"SELECT id, name, file_path, start_row FROM graph_nodes "
		"WHERE project_id=? AND node_type IN (?, ?) AND name IN "
		"(?, ?, ?, ?, ?, ?)";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql.c_str(),
			       static_cast<int>(sql.size()), &stmt,
			       nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"KnowledgeBuilder: prepare entry function query "
			"failed: %s %s\n",
			sqlite3_errmsg(store_->handle()), kLogPrefix);
		return false;
	}

	// Bind parameters: project_id, kind (function), kind (method),
	// then the 6 entry function names.
	int idx = 1;
	sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(project_id_));
	sqlite3_bind_int(stmt, idx++, kKindFunction);
	sqlite3_bind_int(stmt, idx++, kKindMethod);
	for (const auto &name : names)
		sqlite3_bind_text(stmt, idx++, name.c_str(),
				  static_cast<int>(name.size()), SQLITE_STATIC);

	bool ok = true;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		// int64_t entityId = sqlite3_column_int64(stmt, 0);
		const char *nameStr = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *filePath = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		int startRow = sqlite3_column_int(stmt, 3);

		std::string name = nameStr ? nameStr : "";
		std::string path = filePath ? filePath : "";

		// Capability name: "EntryFunction:<function_name>"
		std::string capName = "EntryFunction:" + name;
		// Source ref: "<file_path>:<line_number>"
		std::string sourceRef = path + ":" + std::to_string(startRow);

		if (!store_->insertCapability(project_id_, capName,
					      "Entry function: " + name,
					      "heuristic", sourceRef)) {
			fprintf(stderr,
				"KnowledgeBuilder: insertCapability failed "
				"for %s: %s %s\n",
				capName.c_str(), store_->error().c_str(),
				kLogPrefix);
			ok = false;
		}
	}
	sqlite3_finalize(stmt);
	return ok;
}

// ─── Step 5: TODO/FIXME Comment Scan ──────────────────────────────

/// Query the files table for all source file paths for a project.
/// Returns a vector of (path, language) pairs.
static std::vector<std::pair<std::string, std::string> >
querySourceFiles(store::GraphStore *store, uint64_t project_id)
{
	std::vector<std::pair<std::string, std::string> > files;
	const char *sql = "SELECT path, language FROM files "
			  "WHERE project_id=? ORDER BY path";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"KnowledgeBuilder: prepare source files query "
			"failed: %s %s\n",
			sqlite3_errmsg(store->handle()), kLogPrefix);
		return files;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *p = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *l = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		files.emplace_back(p ? p : "", l ? l : "");
	}
	sqlite3_finalize(stmt);
	return files;
}

/// Check if a line contains a TODO or FIXME comment marker.
/// Returns 0 if no marker found, 1 for TODO, 2 for FIXME.
///
/// To avoid false positives (section headers like "─── TODO/FIXME Comment
/// Scan ───" or prose mentioning TODO as a word), the marker must appear
/// at the start of a comment: immediately after a comment introducer
/// (`//`, `#`, `/*`, or `*` for block-comment continuation) with only
/// optional whitespace between them. Patterns like `// TODO:`, `# TODO:`,
/// `* TODO(Agent 3):` are accepted; `// step 5: TODO scan` is rejected.
static int findTodoOrFixme(const std::string &line, size_t &outPos)
{
	size_t todoPos = line.find("TODO");
	size_t fixmePos = line.find("FIXME");

	// Pick the earliest marker.
	size_t markerPos = std::string::npos;
	int markerType = 0;
	if (todoPos != std::string::npos &&
	    (fixmePos == std::string::npos || todoPos < fixmePos)) {
		markerPos = todoPos;
		markerType = 1;
	} else if (fixmePos != std::string::npos) {
		markerPos = fixmePos;
		markerType = 2;
	} else {
		return 0;
	}

	// Walk backwards from markerPos, skipping only whitespace. The first
	// non-whitespace character must be a comment introducer. This rejects
	// markers buried in prose after a `//` introducer.
	size_t i = markerPos;
	while (i > 0 && std::isspace(static_cast<unsigned char>(line[i - 1])))
		i--;
	if (i == 0)
		return 0; // No introducer before the marker.

	// Reject markers inside backtick-quoted inline code spans (e.g.
	// `// TODO:` in documentation). Count backticks before markerPos;
	// odd count means we are inside an open code span.
	size_t backtickCount = 0;
	for (size_t j = 0; j < markerPos; j++) {
		if (line[j] == '`')
			backtickCount++;
	}
	if (backtickCount % 2 == 1)
		return 0;

	char prev = line[i - 1];
	bool isIntroducer = false;
	if (prev == '#') {
		isIntroducer = true;
	} else if (prev == '*') {
		// Block-comment continuation: ` * TODO:` (multi-line /* */).
		// Also accepts `/* TODO:` since '*' is the last char before TODO.
		isIntroducer = true;
	} else if (prev == '/' && i >= 2 && line[i - 2] == '/') {
		// Line comment: `// TODO`.
		isIntroducer = true;
	}

	if (!isIntroducer)
		return 0;

	outPos = markerPos;
	return markerType;
}

bool KnowledgeBuilder::scanTodoFixme()
{
	auto files = querySourceFiles(store_, project_id_);
	if (files.empty())
		return true;

	bool ok = true;
	for (const auto &[path, language] : files) {
		if (path.empty())
			continue;

		// Skip README files — they are documents, not source code.
		std::string lowerPath = toLower(path);
		if (lowerPath.find("readme") != std::string::npos)
			continue;

		std::string content = readFileContent(path);
		if (content.empty())
			continue;

		auto lines = splitLines(content);
		for (int lineNum = 0; lineNum < static_cast<int>(lines.size());
		     lineNum++) {
			const std::string &line = lines[lineNum];

			size_t markerPos = 0;
			int markerType = findTodoOrFixme(line, markerPos);
			if (markerType == 0)
				continue;

			// Extract the comment text starting from the marker.
			std::string claimText = trim(line.substr(markerPos));

			std::string contractName = (markerType == 2) ? "FIXME" :
								       "TODO";

			if (!store_->insertContract(project_id_, contractName,
						    "comment", claimText, path,
						    lineNum + 1)) {
				fprintf(stderr,
					"KnowledgeBuilder: insertContract "
					"failed for %s: %s %s\n",
					contractName.c_str(),
					store_->error().c_str(), kLogPrefix);
				ok = false;
			}
		}
	}
	return ok;
}

} // namespace knowledge
