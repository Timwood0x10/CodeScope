// gen_defects.cpp — generated-code defect detection (development_plan §8.4, §12.4 M3-2).
//
// All §8.4 detections are L2 observations — candidates, not violations.
// The plan explicitly states: "这些缺陷并非 AI 独有，源码通常也无法证明生成来源"
// (§8.4). We detect four categories:
//
// 1. Dependency declaration anomaly: imports referencing modules not in the
//    project's known module set, excluding standard library and legitimate
//    external packages. Uses arch_edge + arch_module.
// 2. Shell implementation: functions with minimal body. The plan says
//    "不得使用 is_stub" (§8.4) alone — we use a combination of:
//    - body statement count (cyclomatic ≤ 1 = no branches/loops)
//    - call_count == 0 (no calls)
//    - lines ≤ 3 (very short)
//    - is_stub as corroboration (not sole criterion)
// 3. Duplicate implementation: functions across modules with identical
//    metrics signatures (same lines, same cyclomatic, same param_count,
//    same call_count). Structural similarity only — not semantic.
// 4. Invalid abstraction: interfaces/traits with exactly one implementation.
//    Reported as candidate, not defect.

#include "gen_defects.h"

#include "store/store.h"

#include <cstdio>
#include <cstring>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arch
{
namespace
{

/// Check if an import target is a standard library path.
/// Standard library imports vary by language: "std::" (Rust/C++),
/// "fmt/" (Go), "os/" (Go/Python), "sys/" (Python), etc.
bool isStandardLibrary(const std::string &target)
{
	// Strip the include delimiters so "<vector>" and "vector" agree.
	std::string t = target;
	if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
			      (t.front() == '<' && t.back() == '>')))
		t = t.substr(1, t.size() - 2);
	// POSIX system header trees: <sys/socket.h>, <bits/...>, <linux/...>.
	// Without this a standard header with a directory prefix was reported
	// as an unresolved project dependency.
	static const char *const kSystemDirs[] = {
		"sys/",	    "bits/", "linux/", "asm/",	  "asm-generic/",
		"netinet/", "arpa/", "net/",   "machine/"
	};
	for (const char *dir : kSystemDirs)
		if (t.rfind(dir, 0) == 0)
			return true;
	// Rust/C++ standard library.
	if (t.rfind("std::", 0) == 0)
		return true;
	// C standard library headers.
	if (t.rfind("stdio", 0) == 0 || t.rfind("stdlib", 0) == 0 ||
	    t.rfind("string", 0) == 0 || t.rfind("math", 0) == 0 ||
	    t.rfind("ctype", 0) == 0 || t.rfind("time", 0) == 0 ||
	    t.rfind("errno", 0) == 0 || t.rfind("assert", 0) == 0 ||
	    t.rfind("stdint", 0) == 0 || t.rfind("stddef", 0) == 0 ||
	    t.rfind("stdbool", 0) == 0)
		return true;
	// Go standard library.
	if (t.rfind("fmt", 0) == 0 || t.rfind("os", 0) == 0 ||
	    t.rfind("io", 0) == 0 || t.rfind("net", 0) == 0 ||
	    t.rfind("http", 0) == 0 || t.rfind("strings", 0) == 0 ||
	    t.rfind("strconv", 0) == 0 || t.rfind("sort", 0) == 0 ||
	    t.rfind("sync", 0) == 0 || t.rfind("context", 0) == 0 ||
	    t.rfind("errors", 0) == 0 || t.rfind("reflect", 0) == 0 ||
	    t.rfind("encoding", 0) == 0 || t.rfind("path", 0) == 0 ||
	    t.rfind("time", 0) == 0)
		return true;
	// Python standard library.
	if (target.rfind("os", 0) == 0 || target.rfind("sys", 0) == 0 ||
	    target.rfind("json", 0) == 0 || target.rfind("re", 0) == 0 ||
	    target.rfind("typing", 0) == 0 ||
	    target.rfind("collections", 0) == 0 ||
	    target.rfind("functools", 0) == 0 ||
	    target.rfind("itertools", 0) == 0 ||
	    target.rfind("pathlib", 0) == 0 ||
	    target.rfind("dataclasses", 0) == 0 || target.rfind("abc", 0) == 0)
		return true;
	// Node.js built-ins.
	if (target.rfind("node:", 0) == 0)
		return true;
	return false;
}

/// Classify why an unresolved target could not be resolved.
/// Uses the *raw* import target (as written by the developer), not
/// the folded/normalized dst_module, so that a relative import like
/// "../../include/missing.h" is classified by its original syntax
/// even though foldPath collapsed it to an absolute-looking path.
///
/// Returns one of:
/// "bare_header"        — no path separator, likely a header-only
///                         include (e.g. "vector", "stdio.h");
/// "absolute_path"      — POSIX absolute ('/') or Windows drive
///                         letter ('C:\\' or 'C:/') or UNC
///                         ('\\\\server\\share');
/// "cross_dir_relative" — contains '/' or '\\' but is not absolute;
///                         includes folded "../include/x.h" paths
///                         that failed to match a known module.
static std::string classifyUnresolvedReason(const std::string &raw_target)
{
	// An empty raw_target means "not recorded", not a header name: counting
	// it as bare_header would inflate the one bucket that means "likely a
	// system header" with rows that carry no evidence at all.
	if (raw_target.empty())
		return "unknown";

	// POSIX absolute path.
	if (raw_target[0] == '/')
		return "absolute_path";

	// Windows UNC path: \\server\share\...
	if (raw_target.size() >= 2 && raw_target[0] == '\\' &&
	    raw_target[1] == '\\')
		return "absolute_path";

	// Windows drive-letter path: C:\... or C:/...
	if (raw_target.size() >= 3 &&
	    ((raw_target[0] >= 'A' && raw_target[0] <= 'Z') ||
	     (raw_target[0] >= 'a' && raw_target[0] <= 'z')) &&
	    raw_target[1] == ':' &&
	    (raw_target[2] == '\\' || raw_target[2] == '/'))
		return "absolute_path";

	// No path separator at all → bare header name.
	if (raw_target.find('/') == std::string::npos &&
	    raw_target.find('\\') == std::string::npos)
		return "bare_header";

	return "cross_dir_relative";
}

/// Collect all arch_edge rows for the project and build a vector of
/// (resolution_kind, src_module, dst_module, source_file, source_row).
struct EdgeRow {
	std::string resolution_kind;
	std::string src_module;
	std::string dst_module;
	std::string raw_target;
	std::string source_file;
	int source_row;
};

static bool loadAllEdges(sqlite3 *db, uint64_t project_id,
			 std::vector<EdgeRow> &out)
{
	const char *sql = "SELECT resolution_kind, src_module, dst_module, "
			  "raw_target, source_file, source_row "
			  "FROM arch_edge WHERE project_id=?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		EdgeRow row;
		auto text = [&](int col) -> std::string {
			const unsigned char *t = sqlite3_column_text(stmt, col);
			return t ? reinterpret_cast<const char *>(t) : "";
		};
		row.resolution_kind = text(0);
		row.src_module = text(1);
		row.dst_module = text(2);
		row.raw_target = text(3);
		row.source_file = text(4);
		row.source_row = sqlite3_column_int(stmt, 5);
		out.push_back(std::move(row));
	}
	sqlite3_finalize(stmt);
	return true;
}

/// Build DependencyStats from a list of edge rows.
static DependencyStats buildDepStats(const std::vector<EdgeRow> &edges)
{
	DependencyStats stats;
	stats.total = static_cast<int>(edges.size());
	for (const auto &e : edges) {
		if (e.resolution_kind == "internal")
			++stats.internal_count;
		else if (e.resolution_kind == "external")
			++stats.external_count;
		else if (e.resolution_kind == "unresolved") {
			++stats.unresolved_count;
			// Classify from raw_target only: dst_module is folded and,
			// when raw_target is empty, using it would turn "not
			// recorded" into a confident cross-dir verdict.
			std::string reason =
				classifyUnresolvedReason(e.raw_target);
			if (reason == "bare_header")
				++stats.bare_header_count;
			else if (reason == "absolute_path")
				++stats.absolute_path_count;
			else if (reason == "unknown")
				++stats.unknown_count;
			else
				++stats.cross_dir_relative_count;
		}
	}
	if (stats.total > 0) {
		stats.unresolved_ratio =
			static_cast<double>(stats.unresolved_count) /
			stats.total;
		stats.external_ratio =
			static_cast<double>(stats.external_count) / stats.total;
		stats.internal_ratio =
			static_cast<double>(stats.internal_count) / stats.total;
	}
	return stats;
}

/// Emit a dependency_anomaly candidate for one unresolved edge.
static void emitAnomalyCandidate(const EdgeRow &e, DefectResult &result)
{
	// Classify and gate from raw_target only: dst_module is folded, so a
	// folded path looks project-relative and "<sys/types.h>" would be
	// reported as a dependency anomaly.
	const std::string &raw = e.raw_target;
	if (isStandardLibrary(raw))
		return;
	std::string reason = classifyUnresolvedReason(raw);

	DefectCandidate d;
	d.category = "dependency_anomaly";
	d.target = e.dst_module;
	d.src_module = e.src_module;
	d.file_path = e.source_file;
	d.row = e.source_row;
	// How strongly the reason points at a declaration problem:
	// a project-internal path no module matches is the strongest signal,
	// an absolute path is often external, and "unknown" has no evidence.
	d.confidence = reason == "cross_dir_relative" ? 0.5 :
		       reason == "absolute_path"      ? 0.2 :
							0.3;
	d.reason = reason;
	d.description = "Unresolved import: module '" + e.src_module +
			"' imports '" + e.dst_module +
			"' which cannot be resolved to any known module "
			"or external package [reason: " +
			reason + "]";
	d.evidence_basis =
		"arch_edge.resolution_kind='unresolved'; module_pair=" +
		e.src_module + " -> " + e.dst_module;
	result.candidates.push_back(std::move(d));
}

/// Detect dependency declaration anomalies (§12.4 M3-2).
///
/// An import is anomalous if its resolution_kind is 'unresolved' —
/// meaning the target path contains a separator (so it looks like a
/// file path) but does not match any known project module. Edges
/// marked 'external' (third-party packages, standard library) are
/// not anomalies.
///
/// For each unresolved edge we:
/// - classify the reason (bare_header / absolute_path /
///   cross_dir_relative);
/// - emit a candidate with source_file + source_row + module pair;
/// - aggregate counts and ratios into DependencyStats.
///
/// Foldable relative paths ("./x", "../x") are already collapsed by
/// resolveRelativeTarget + foldPath in module_graph.cpp before the
/// edge is written, so they should never appear as unresolved here.
void detectDependencyAnomalies(store::GraphStore &store, uint64_t project_id,
			       DefectResult &result)
{
	sqlite3 *db = store.handle();
	if (!db)
		return;

	std::vector<EdgeRow> edges;
	if (!loadAllEdges(db, project_id, edges))
		return;

	result.dep_stats = buildDepStats(edges);

	for (const auto &e : edges) {
		if (e.resolution_kind == "unresolved")
			emitAnomalyCandidate(e, result);
	}
}

/// One function's metrics for shell/duplicate detection.
struct FunctionMetrics {
	int64_t entity_id;
	std::string name;
	std::string file_path;
	std::string language;
	int cyclomatic;
	int lines;
	int param_count;
	int call_count;
	int branch_count;
	int loop_count;
	int is_stub;
};

/// Detect shell implementations.
/// Criteria (§8.4): combination of:
/// - body statement count (cyclomatic ≤ 1 = no branches/loops)
/// - call_count == 0 (no function calls in body)
/// - lines ≤ 3 (very short body)
/// - is_stub as corroboration (not sole criterion)
///
/// A function is a shell candidate if it meets ALL of:
///   cyclomatic ≤ 1 AND call_count == 0 AND lines ≤ 3 AND is_stub == 1
/// OR:
///   cyclomatic ≤ 1 AND call_count == 0 AND lines ≤ 2
/// (the second path catches shells even without is_stub)
void detectShellImplementations(store::GraphStore &store, uint64_t project_id,
				DefectResult &result)
{
	sqlite3 *db = store.handle();
	if (!db)
		return;

	const char *sql =
		"SELECT e.id, e.name, e.file_path, e.language, "
		"e.cyclomatic, e.lines, e.param_count, e.call_count, "
		"e.branch_count, e.loop_count, e.is_stub "
		"FROM entity e WHERE e.project_id=? AND e.kind IN (0,1) "
		"AND e.cyclomatic > 0";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		FunctionMetrics fm;
		fm.entity_id = sqlite3_column_int64(stmt, 0);
		const unsigned char *name = sqlite3_column_text(stmt, 1);
		const unsigned char *fpath = sqlite3_column_text(stmt, 2);
		const unsigned char *lang = sqlite3_column_text(stmt, 3);
		fm.cyclomatic = sqlite3_column_int(stmt, 4);
		fm.lines = sqlite3_column_int(stmt, 5);
		fm.param_count = sqlite3_column_int(stmt, 6);
		fm.call_count = sqlite3_column_int(stmt, 7);
		fm.branch_count = sqlite3_column_int(stmt, 8);
		fm.loop_count = sqlite3_column_int(stmt, 9);
		fm.is_stub = sqlite3_column_int(stmt, 10);
		fm.name = name ? reinterpret_cast<const char *>(name) : "";
		fm.file_path = fpath ? reinterpret_cast<const char *>(fpath) :
				       "";
		fm.language = lang ? reinterpret_cast<const char *>(lang) : "";

		// Shell criteria: minimal body, no calls, no branches.
		bool is_shell = false;
		double confidence = 0.0;

		if (fm.cyclomatic <= 1 && fm.call_count == 0 && fm.lines <= 3 &&
		    fm.is_stub) {
			is_shell = true;
			confidence = 0.6; // Strong: all four criteria.
		} else if (fm.cyclomatic <= 1 && fm.call_count == 0 &&
			   fm.lines <= 2) {
			is_shell = true;
			confidence = 0.4; // Moderate: three criteria, no stub.
		}

		if (is_shell) {
			DefectCandidate d;
			d.category = "shell_implementation";
			d.target = fm.name;
			d.file_path = fm.file_path;
			d.confidence = confidence;
			d.description =
				"Function '" + fm.name +
				"' has a shell implementation: cyclomatic=" +
				std::to_string(fm.cyclomatic) +
				", call_count=" +
				std::to_string(fm.call_count) +
				", lines=" + std::to_string(fm.lines) +
				", is_stub=" + std::to_string(fm.is_stub);
			d.evidence_basis = "entity metrics: cyclomatic≤1 AND "
					   "call_count=0 AND lines≤3";
			result.candidates.push_back(std::move(d));
		}
	}
	sqlite3_finalize(stmt);
}

/// Detect duplicate implementations across modules.
/// Two functions are duplicates if they have identical:
/// - lines (±1)
/// - cyclomatic
/// - param_count
/// - call_count
/// AND are in different file paths (different modules).
void detectDuplicateImplementations(store::GraphStore &store,
				    uint64_t project_id, DefectResult &result)
{
	sqlite3 *db = store.handle();
	if (!db)
		return;

	// Load all function metrics.
	const char *sql =
		"SELECT e.id, e.name, e.file_path, e.language, "
		"e.cyclomatic, e.lines, e.param_count, e.call_count "
		"FROM entity e WHERE e.project_id=? AND e.kind IN (0,1) "
		"AND e.cyclomatic > 0 AND e.lines >= 5";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	std::vector<FunctionMetrics> functions;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		FunctionMetrics fm;
		fm.entity_id = sqlite3_column_int64(stmt, 0);
		const unsigned char *name = sqlite3_column_text(stmt, 1);
		const unsigned char *fpath = sqlite3_column_text(stmt, 2);
		const unsigned char *lang = sqlite3_column_text(stmt, 3);
		fm.cyclomatic = sqlite3_column_int(stmt, 4);
		fm.lines = sqlite3_column_int(stmt, 5);
		fm.param_count = sqlite3_column_int(stmt, 6);
		fm.call_count = sqlite3_column_int(stmt, 7);
		fm.name = name ? reinterpret_cast<const char *>(name) : "";
		fm.file_path = fpath ? reinterpret_cast<const char *>(fpath) :
				       "";
		fm.language = lang ? reinterpret_cast<const char *>(lang) : "";
		functions.push_back(std::move(fm));
	}
	sqlite3_finalize(stmt);

	// Group by metrics signature for duplicate detection.
	// Key: (cyclomatic, lines, param_count, call_count)
	struct MetricsKey {
		int cyclomatic;
		int lines;
		int params;
		int calls;
		bool operator==(const MetricsKey &o) const
		{
			return cyclomatic == o.cyclomatic && lines == o.lines &&
			       params == o.params && calls == o.calls;
		}
	};

	struct MetricsKeyHash {
		size_t operator()(const MetricsKey &k) const
		{
			return std::hash<int>()(k.cyclomatic) ^
			       (std::hash<int>()(k.lines) << 1) ^
			       (std::hash<int>()(k.params) << 2) ^
			       (std::hash<int>()(k.calls) << 3);
		}
	};

	std::unordered_map<MetricsKey, std::vector<size_t>, MetricsKeyHash>
		groups;

	for (size_t i = 0; i < functions.size(); ++i) {
		MetricsKey key{ functions[i].cyclomatic, functions[i].lines,
				functions[i].param_count,
				functions[i].call_count };
		groups[key].push_back(i);
	}

	// Report groups with 2+ functions in different files.
	for (const auto &kv : groups) {
		if (kv.second.size() < 2)
			continue;

		// Check that at least two are in different files.
		std::unordered_set<std::string> files;
		for (size_t idx : kv.second)
			files.insert(functions[idx].file_path);
		if (files.size() < 2)
			continue;

		// Report the first pair as a candidate.
		const auto &f1 = functions[kv.second[0]];
		const auto &f2 = functions[kv.second[1]];

		DefectCandidate d;
		d.category = "duplicate_implementation";
		d.target = f1.name + " / " + f2.name;
		d.file_path = f1.file_path;
		d.confidence = 0.3; // Low: structural similarity only.
		d.description =
			"Possible duplicate implementation: '" + f1.name +
			"' (" + f1.file_path + ") and '" + f2.name + "' (" +
			f2.file_path + ") have identical metrics: lines=" +
			std::to_string(f1.lines) +
			", cyclomatic=" + std::to_string(f1.cyclomatic) +
			", params=" + std::to_string(f1.param_count) +
			", calls=" + std::to_string(f1.call_count);
		d.evidence_basis =
			"structural similarity: identical metrics signature";
		result.candidates.push_back(std::move(d));
	}
}

/// Detect invalid abstractions: interfaces/traits with exactly one
/// implementation. Reported as candidates, not defects (§8.4).
void detectInvalidAbstractions(store::GraphStore &store, uint64_t project_id,
			       DefectResult &result)
{
	sqlite3 *db = store.handle();
	if (!db)
		return;

	// Find interface/trait/abstract entities (kind=2 for class/interface).
	// Then check how many implementations each has via the
	// interface_impl table or relation table.
	//
	// For now, we use a simple heuristic: find entities of kind=2
	// (class/interface) and check if they have exactly one implementor
	// in the relation table (type=6 for IMPLEMENTS, if available).
	//
	// If the relation type for implements is not available, we skip
	// this detection — it's a candidate, not a required detection.

	// Check if the interface_impl table exists.
	sqlite3_stmt *check = nullptr;
	bool has_impl_table = false;
	if (sqlite3_prepare_v2(db,
			       "SELECT name FROM sqlite_master "
			       "WHERE type='table' AND name='interface_impl'",
			       -1, &check, nullptr) == SQLITE_OK) {
		if (sqlite3_step(check) == SQLITE_ROW)
			has_impl_table = true;
		sqlite3_finalize(check);
	}

	if (!has_impl_table)
		return;

	// Find interfaces with exactly one implementation.
	const char *sql =
		"SELECT e.name, e.file_path, "
		"(SELECT COUNT(*) FROM interface_impl ii "
		" WHERE ii.project_id=? AND ii.interface_id=e.id) AS impl_count "
		"FROM entity e WHERE e.project_id=? AND e.kind=2 "
		"HAVING impl_count = 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return;

	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *name = sqlite3_column_text(stmt, 0);
		const unsigned char *fpath = sqlite3_column_text(stmt, 1);

		std::string iface_name =
			name ? reinterpret_cast<const char *>(name) : "";
		std::string file_path =
			fpath ? reinterpret_cast<const char *>(fpath) : "";

		DefectCandidate d;
		d.category = "invalid_abstraction";
		d.target = iface_name;
		d.file_path = file_path;
		d.confidence = 0.3;
		d.description =
			"Interface/trait '" + iface_name +
			"' has exactly one implementation — potential "
			"unnecessary abstraction (candidate, not defect)";
		d.evidence_basis = "interface_impl count = 1";
		result.candidates.push_back(std::move(d));
	}
	sqlite3_finalize(stmt);
}

} // namespace

DefectResult detectGenDefects(store::GraphStore &store, uint64_t project_id)
{
	DefectResult result;

	detectDependencyAnomalies(store, project_id, result);
	detectShellImplementations(store, project_id, result);
	detectDuplicateImplementations(store, project_id, result);
	detectInvalidAbstractions(store, project_id, result);

	return result;
}

} // namespace arch
