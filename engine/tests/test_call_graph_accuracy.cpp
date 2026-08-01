// test_call_graph_accuracy.cpp
//
// Step 2 — Quantifiable Accuracy Benchmark (plan §Step 2).
//
// For each portable fixture under engine/tests/accuracy/fixtures/<lang>/:
//   1. Index the fixture directory with CODESCOPE_SKIP_ASYNC=1 so the
//      result is deterministic (no background model/FTS/state threads).
//   2. Enumerate the actual CALLS set from SQLite (relation type=1
//      JOINed with entity on both endpoints).
//   3. Load the fixture's ground_truth.json (expected_calls,
//      forbidden_calls, allowed_unresolved, external_calls).
//   4. Compute TP/FP/FN/Precision/Recall/F1 using semantic identity
//      (caller_name + caller_file_basename → callee_name +
//      callee_file_basename). No database IDs are used.
//   5. Aggregate overall + per-language metrics.
//
// Outputs:
//   - /tmp/codescope_accuracy_report.json (machine-readable)
//   - stderr (same JSON for CI capture)
//
// Gate: returns nonzero if any fixture has FP > 0 or FN > 0.
//
// Fault injection (verification of the gate, plan §Step 2.8):
//   - CODESCOPE_INJECT_FP=1: adds one fake edge to every fixture's
//     actual set. Precision must drop and the exit code must be nonzero.
//   - CODESCOPE_INJECT_FN=1: removes one expected edge from every
//     fixture's actual set (simulating a missed call). Recall must drop
//     and the exit code must be nonzero.
//   These hooks exist solely to prove the gate catches regressions; they
//   are off by default.

#include "../include/engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

// ─── Minimal JSON parser for ground_truth.json ───────────────────────
// The ground_truth format is fixed and simple: objects with string keys
// and string values, arrays of objects, arrays of strings. This parser
// handles exactly that subset — it is NOT a general JSON library.

namespace
{

/// A parsed JSON value. Only the variants needed by ground_truth are
/// modelled: string, array (of values), object (string→value).
struct JsonValue {
	enum class Type { String, Array, Object };
	Type type = Type::String;
	std::string str;
	std::vector<JsonValue> arr;
	std::vector<std::pair<std::string, JsonValue>> obj;
};

/// Skip whitespace in the JSON text.
void skipWs(const std::string &s, size_t &i)
{
	while (i < s.size() &&
	       (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
		++i;
}

/// Parse a JSON string literal starting at s[i] (s[i] == '"').
std::string parseString(const std::string &s, size_t &i)
{
	std::string out;
	if (i >= s.size() || s[i] != '"')
		return out;
	++i;
	while (i < s.size() && s[i] != '"') {
		if (s[i] == '\\' && i + 1 < s.size()) {
			char c = s[i + 1];
			if (c == 'n')
				out += '\n';
			else if (c == 't')
				out += '\t';
			else
				out += c;
			i += 2;
		} else {
			out += s[i++];
		}
	}
	if (i < s.size())
		++i; // closing quote
	return out;
}

/// Recursive descent parser. Returns a JsonValue; on malformed input it
/// returns a String-typed value with empty content (good enough for the
/// fixed ground_truth format which is hand-authored and validated).
JsonValue parseValue(const std::string &s, size_t &i)
{
	skipWs(s, i);
	if (i >= s.size())
		return {};
	if (s[i] == '"') {
		JsonValue v;
		v.type = JsonValue::Type::String;
		v.str = parseString(s, i);
		return v;
	}
	if (s[i] == '[') {
		JsonValue v;
		v.type = JsonValue::Type::Array;
		++i;
		skipWs(s, i);
		if (i < s.size() && s[i] == ']') {
			++i;
			return v;
		}
		while (i < s.size()) {
			v.arr.push_back(parseValue(s, i));
			skipWs(s, i);
			if (i < s.size() && s[i] == ',') {
				++i;
				continue;
			}
			break;
		}
		skipWs(s, i);
		if (i < s.size() && s[i] == ']')
			++i;
		return v;
	}
	if (s[i] == '{') {
		JsonValue v;
		v.type = JsonValue::Type::Object;
		++i;
		skipWs(s, i);
		if (i < s.size() && s[i] == '}') {
			++i;
			return v;
		}
		while (i < s.size()) {
			skipWs(s, i);
			if (i >= s.size() || s[i] != '"')
				break;
			std::string key = parseString(s, i);
			skipWs(s, i);
			if (i < s.size() && s[i] == ':')
				++i;
			v.obj.emplace_back(key, parseValue(s, i));
			skipWs(s, i);
			if (i < s.size() && s[i] == ',') {
				++i;
				continue;
			}
			break;
		}
		skipWs(s, i);
		if (i < s.size() && s[i] == '}')
			++i;
		return v;
	}
	// Numbers / true / false / null are not used in ground_truth; skip.
	while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
		++i;
	return {};
}

/// Parse a full JSON document.
JsonValue parseJson(const std::string &s)
{
	size_t i = 0;
	return parseValue(s, i);
}

/// Look up a string field in a JSON object, returning "" if absent.
std::string getField(const JsonValue &obj, const std::string &key)
{
	for (const auto &[k, v] : obj.obj)
		if (k == key && v.type == JsonValue::Type::String)
			return v.str;
	return "";
}

/// Look up an array field in a JSON object by key. Returns nullptr if
/// the key is absent or the value is not an array.
const JsonValue *getArrayField(const JsonValue &obj, const std::string &key)
{
	for (const auto &[k, v] : obj.obj)
		if (k == key && v.type == JsonValue::Type::Array)
			return &v;
	return nullptr;
}

} // namespace

// ─── Call-edge identity ──────────────────────────────────────────────

/// A semantic call edge identity. Uses names + file basenames only — no
/// database IDs — so it is stable across re-indexes and DB rebuilds.
struct CallEdge {
	std::string caller;
	std::string caller_file;
	std::string callee;
	std::string callee_file;

	/// Canonical key string for set membership: "caller@file -> callee@file".
	std::string key() const
	{
		return caller + "@" + caller_file + " -> " + callee + "@" +
		       callee_file;
	}
};

/// Extract the basename from a path. The fixtures use relative paths
/// like "a.go"; the engine stores absolute paths. Comparing basenames
/// keeps the identity portable.
static std::string basenameOf(const std::string &path)
{
	size_t pos = path.find_last_of("/\\");
	return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// ─── Per-fixture result ──────────────────────────────────────────────

struct FixtureResult {
	std::string language;
	int tp = 0;
	int fp = 0;
	int fn = 0;
	double precision = 0.0;
	double recall = 0.0;
	double f1 = 0.0;
	std::vector<std::string> false_positives;
	std::vector<std::string> false_negatives;
};

/// Compute precision/recall/F1 from TP/FP/FN. F1 is 0 when TP == 0.
static void computeMetrics(FixtureResult &r)
{
	r.precision = (r.tp + r.fp > 0) ?
			      static_cast<double>(r.tp) / (r.tp + r.fp) :
			      0.0;
	r.recall = (r.tp + r.fn > 0) ?
			   static_cast<double>(r.tp) / (r.tp + r.fn) :
			   0.0;
	r.f1 = (r.precision + r.recall > 0) ?
		       2.0 * r.precision * r.recall / (r.precision + r.recall) :
		       0.0;
}

// ─── Actual CALLS enumeration ────────────────────────────────────────

/// Enumerate all actual CALLS edges for a project from SQLite. Each edge
/// is identified by caller/callee name + file basename (semantic
/// identity, no DB IDs).
static std::set<std::string> enumerateActualCalls(sqlite3 *db,
						  uint64_t project_id)
{
	std::set<std::string> actual;
	const char *sql = "SELECT caller.name, caller.file_path, callee.name, "
			  "callee.file_path FROM relation r "
			  "JOIN entity caller ON r.source_id = caller.id "
			  "JOIN entity callee ON r.target_id = callee.id "
			  "WHERE r.project_id = ? AND r.type = 1";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		sqlite3_finalize(stmt);
		return actual;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		CallEdge e;
		const char *cn = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *cf = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *ee = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *ef = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));
		e.caller = cn ? cn : "";
		e.caller_file = basenameOf(cf ? cf : "");
		e.callee = ee ? ee : "";
		e.callee_file = basenameOf(ef ? ef : "");
		// Skip edges where either endpoint name is empty (malformed).
		if (!e.caller.empty() && !e.callee.empty())
			actual.insert(e.key());
	}
	sqlite3_finalize(stmt);
	return actual;
}

// ─── Ground-truth loading ────────────────────────────────────────────

/// Load and parse a fixture's ground_truth.json. Returns false on read
/// error.
static bool loadGroundTruth(const std::string &path, JsonValue &out)
{
	std::ifstream f(path);
	if (!f)
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	out = parseJson(ss.str());
	return out.type == JsonValue::Type::Object;
}

/// Build a set of exact edge keys from a JSON array of call objects
/// (looked up by `key` in `obj`). Each object has caller, caller_file,
/// callee, callee_file. Edges with missing file fields are skipped
/// because the fixtures always provide them; skipping incomplete edges
/// avoids wildcard-matching ambiguity.
static std::set<std::string> buildEdgeSet(const JsonValue &obj,
					  const std::string &key)
{
	std::set<std::string> set;
	const JsonValue *arr = getArrayField(obj, key);
	if (!arr)
		return set;
	for (const auto &item : arr->arr) {
		if (item.type != JsonValue::Type::Object)
			continue;
		CallEdge e;
		e.caller = getField(item, "caller");
		e.caller_file = getField(item, "caller_file");
		e.callee = getField(item, "callee");
		e.callee_file = getField(item, "callee_file");
		if (e.caller.empty() || e.caller_file.empty() ||
		    e.callee.empty() || e.callee_file.empty())
			continue; // skip incomplete edges
		set.insert(e.key());
	}
	return set;
}

/// Build a set of loose edge keys (caller + callee only, ignoring file)
/// for forbidden/allowed_unresolved entries that may omit file fields.
/// The loose key format is "caller@* -> callee@*" so a wildcard match
/// can be performed against actual keys.
static std::set<std::string> buildLooseEdgeSet(const JsonValue &obj,
					       const std::string &key)
{
	std::set<std::string> set;
	const JsonValue *arr = getArrayField(obj, key);
	if (!arr)
		return set;
	for (const auto &item : arr->arr) {
		if (item.type != JsonValue::Type::Object)
			continue;
		std::string caller = getField(item, "caller");
		std::string callee = getField(item, "callee");
		if (caller.empty() || callee.empty())
			continue;
		// Loose key: match any file. We store caller@* -> callee@* so
		// the membership check can use a wildcard file.
		set.insert(caller + "@* -> " + callee + "@*");
	}
	return set;
}

/// Check whether an actual edge key matches a loose key (caller@* ->
/// callee@*). The actual key is "caller@file -> callee@file".
static bool matchesLoose(const std::string &actual_key,
			 const std::set<std::string> &loose)
{
	// Extract caller and callee names from the actual key.
	// Format: "caller@file -> callee@file"
	size_t arrow = actual_key.find(" -> ");
	if (arrow == std::string::npos)
		return false;
	std::string left = actual_key.substr(0, arrow);
	std::string right = actual_key.substr(arrow + 4);
	size_t at_l = left.find('@');
	size_t at_r = right.find('@');
	std::string caller = (at_l == std::string::npos) ? left :
							   left.substr(0, at_l);
	std::string callee =
		(at_r == std::string::npos) ? right : right.substr(0, at_r);
	std::string loose_key = caller + "@* -> " + callee + "@*";
	return loose.count(loose_key) > 0;
}

// ─── Fixture runner ──────────────────────────────────────────────────

/// Resolve the fixtures root directory. Tries (1) env
/// CODESCOPE_ACCURACY_FIXTURES, (2) a path derived from __FILE__, (3) a
/// relative fallback.
static std::string resolveFixturesRoot()
{
	const char *env = getenv("CODESCOPE_ACCURACY_FIXTURES");
	if (env && *env)
		return env;
	// __FILE__ is engine/tests/test_call_graph_accuracy.cpp at build
	// time. The fixtures live in engine/tests/accuracy/fixtures/.
	std::string file = __FILE__;
	size_t slash = file.find_last_of("/\\");
	if (slash != std::string::npos) {
		std::string dir = file.substr(0, slash); // engine/tests
		return dir + "/accuracy/fixtures";
	}
	return "engine/tests/accuracy/fixtures";
}

/// Run one fixture: index, enumerate actual calls, compare against
/// ground truth, return a FixtureResult.
///
/// The fixture source files live under engine/tests/accuracy/fixtures/.
/// The engine's FilterPolicy skips any path containing a "tests" path
/// component (normal_skip_dirs_), so indexing the fixture directory
/// in-place yields zero entities. To work around this, the fixture is
/// copied to a temp directory whose path does not contain any skip-list
/// component before indexing.
static FixtureResult runFixture(const std::string &fixture_dir, bool inject_fp,
				bool inject_fn)
{
	FixtureResult r;
	std::string gt_path = fixture_dir + "/ground_truth.json";
	JsonValue gt;
	if (!loadGroundTruth(gt_path, gt)) {
		fprintf(stderr,
			"FAIL: cannot load ground_truth.json at %s "
			"[module=test_call_graph_accuracy, method=runFixture]\n",
			gt_path.c_str());
		r.fn = -1; // signal error
		return r;
	}
	r.language = getField(gt, "language");

	// Copy the fixture to a temp directory whose path avoids the
	// FilterPolicy skip list (e.g. "tests"). Only copy source files,
	// not ground_truth.json, so the JSON is not mistaken for source.
	std::string tmp_dir = "/tmp/codescope_acc_src_" + r.language;
	std::filesystem::remove_all(tmp_dir);
	std::filesystem::create_directories(tmp_dir);
	for (const auto &entry :
	     std::filesystem::directory_iterator(fixture_dir)) {
		if (!entry.is_regular_file())
			continue;
		std::string name = entry.path().filename().string();
		if (name == "ground_truth.json")
			continue;
		std::filesystem::copy_file(
			entry.path(), tmp_dir + "/" + name,
			std::filesystem::copy_options::overwrite_existing);
	}

	// Index the fixture in a fresh DB.
	char db_path[256];
	snprintf(db_path, sizeof(db_path), "/tmp/codescope_accuracy_%s.db",
		 r.language.c_str());
	unlink(db_path);
	std::string lbug_path = std::string(db_path);
	lbug_path.replace(lbug_path.size() - 3, 3, ".lbug");
	unlink(lbug_path.c_str());

	if (engine_init(db_path) != 0) {
		fprintf(stderr, "FAIL: engine_init for %s\n",
			r.language.c_str());
		r.fn = -1;
		return r;
	}
	uint64_t pid = engine_create_project(tmp_dir.c_str(),
					     ("acc-" + r.language).c_str());
	if (pid == 0) {
		fprintf(stderr, "FAIL: create_project for %s\n",
			r.language.c_str());
		engine_shutdown();
		r.fn = -1;
		return r;
	}
	char *idx = engine_index_project(pid, tmp_dir.c_str(), nullptr);
	if (!idx || !strstr(idx, "\"ok\":true")) {
		fprintf(stderr, "FAIL: index_project for %s: %s\n",
			r.language.c_str(), idx ? idx : "(null)");
		engine_free_string(idx);
		engine_shutdown();
		r.fn = -1;
		return r;
	}
	engine_free_string(idx);

	// Allow the synchronous LadybugDB compile to settle.
	usleep(150000);

	// Enumerate actual CALLS.
	sqlite3 *db = nullptr;
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: sqlite3_open for %s\n",
			r.language.c_str());
		engine_shutdown();
		r.fn = -1;
		return r;
	}
	std::set<std::string> actual = enumerateActualCalls(db, pid);
	sqlite3_close(db);
	engine_shutdown();

	// Build ground-truth sets.
	std::set<std::string> expected = buildEdgeSet(gt, "expected_calls");
	std::set<std::string> forbidden =
		buildLooseEdgeSet(gt, "forbidden_calls");
	std::set<std::string> allowed_unresolved_loose =
		buildLooseEdgeSet(gt, "allowed_unresolved");

	// External calls: a set of callee names that are third-party/builtin.
	// These should NOT map to internal project entities; if one leaks
	// into the actual set (entity-joined), it is ignored only when it is
	// NOT also a forbidden call (forbidden takes precedence).
	std::set<std::string> external_names;
	if (const JsonValue *ex = getArrayField(gt, "external_calls")) {
		for (const auto &item : ex->arr)
			if (item.type == JsonValue::Type::String)
				external_names.insert(item.str);
	}

	// Fault injection: add a fake edge (FP) or remove an expected edge
	// from actual (FN). FP injection adds a spurious edge that is not in
	// expected → precision drops. FN injection removes a real expected
	// edge from actual → recall drops. Both must cause nonzero exit.
	if (inject_fp) {
		actual.insert("__fake_caller@fake.go -> __fake_callee@fake.go");
	}
	if (inject_fn && !expected.empty()) {
		std::string victim = *expected.begin();
		actual.erase(victim);
	}

	// TP = |actual ∩ expected|
	for (const auto &e : expected)
		if (actual.count(e))
			++r.tp;

	// FN = expected - actual
	for (const auto &e : expected)
		if (!actual.count(e)) {
			++r.fn;
			r.false_negatives.push_back(e);
		}

	// FP = actual - expected - (allowed_unresolved) - (external)
	// Precedence for an actual edge not in expected:
	//   1. Forbidden (loose match) → always FP (hard rule: must not exist).
	//   2. Allowed unresolved (loose match) → not FP (uncertain call).
	//   3. External (callee name match) → not FP (builtin/third-party).
	//   4. Otherwise → FP.
	for (const auto &e : actual) {
		if (expected.count(e))
			continue;
		// Forbidden edges that appear are always FP.
		if (matchesLoose(e, forbidden)) {
			++r.fp;
			r.false_positives.push_back(e);
			continue;
		}
		if (matchesLoose(e, allowed_unresolved_loose))
			continue;
		// External: if the callee name matches any external_calls
		// entry, treat as ignored (not FP).
		bool is_external = false;
		size_t arrow = e.find(" -> ");
		if (arrow != std::string::npos) {
			std::string right = e.substr(arrow + 4);
			size_t at = right.find('@');
			std::string callee_name = (at == std::string::npos) ?
							  right :
							  right.substr(0, at);
			for (const auto &ext : external_names) {
				// External entries may be like "fmt.Println" or
				// "len"; match if the callee name equals the last
				// component.
				std::string ext_last = ext;
				size_t dot = ext_last.find_last_of(".:");
				if (dot != std::string::npos)
					ext_last = ext_last.substr(dot + 1);
				if (callee_name == ext_last ||
				    callee_name == ext) {
					is_external = true;
					break;
				}
			}
		}
		if (is_external)
			continue;
		++r.fp;
		r.false_positives.push_back(e);
	}

	computeMetrics(r);
	return r;
}

int main()
{
	// Disable async enhancement so results are deterministic (plan
	// §Step 2.6 / A11). The synchronous index path builds the full
	// call graph; async model/FTS/state work is skipped.
	setenv("CODESCOPE_SKIP_ASYNC", "1", 1);

	bool inject_fp = getenv("CODESCOPE_INJECT_FP") &&
			 getenv("CODESCOPE_INJECT_FP")[0] == '1';
	bool inject_fn = getenv("CODESCOPE_INJECT_FN") &&
			 getenv("CODESCOPE_INJECT_FN")[0] == '1';

	std::string root = resolveFixturesRoot();
	std::vector<std::string> fixture_dirs;
	for (const auto &entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_directory())
			fixture_dirs.push_back(entry.path().string());
	}
	std::sort(fixture_dirs.begin(), fixture_dirs.end());

	std::vector<FixtureResult> results;
	int overall_tp = 0, overall_fp = 0, overall_fn = 0;
	bool had_error = false;
	for (const auto &dir : fixture_dirs) {
		FixtureResult r = runFixture(dir, inject_fp, inject_fn);
		if (r.fn < 0) {
			had_error = true;
			continue;
		}
		overall_tp += r.tp;
		overall_fp += r.fp;
		overall_fn += r.fn;
		results.push_back(r);
	}

	double overall_p =
		(overall_tp + overall_fp > 0) ?
			(double)overall_tp / (overall_tp + overall_fp) :
			0.0;
	double overall_r =
		(overall_tp + overall_fn > 0) ?
			(double)overall_tp / (overall_tp + overall_fn) :
			0.0;
	double overall_f1 =
		(overall_p + overall_r > 0) ?
			2.0 * overall_p * overall_r / (overall_p + overall_r) :
			0.0;

	// Emit JSON report.
	std::string json;
	json += "{\n";
	json += "  \"schema_version\": 1,\n";
	json += "  \"step\": 2,\n";
	json += "  \"inject_fp\": " +
		std::string(inject_fp ? "true" : "false") + ",\n";
	json += "  \"inject_fn\": " +
		std::string(inject_fn ? "true" : "false") + ",\n";
	json += "  \"overall\": {\n";
	json += "    \"tp\": " + std::to_string(overall_tp) + ",\n";
	json += "    \"fp\": " + std::to_string(overall_fp) + ",\n";
	json += "    \"fn\": " + std::to_string(overall_fn) + ",\n";
	json += "    \"precision\": " + std::to_string(overall_p) + ",\n";
	json += "    \"recall\": " + std::to_string(overall_r) + ",\n";
	json += "    \"f1\": " + std::to_string(overall_f1) + "\n";
	json += "  },\n";
	json += "  \"per_language\": [\n";
	for (size_t i = 0; i < results.size(); ++i) {
		const auto &r = results[i];
		json += "    {\n";
		json += "      \"language\": \"" + r.language + "\",\n";
		json += "      \"tp\": " + std::to_string(r.tp) + ",\n";
		json += "      \"fp\": " + std::to_string(r.fp) + ",\n";
		json += "      \"fn\": " + std::to_string(r.fn) + ",\n";
		json += "      \"precision\": " + std::to_string(r.precision) +
			",\n";
		json += "      \"recall\": " + std::to_string(r.recall) + ",\n";
		json += "      \"f1\": " + std::to_string(r.f1) + ",\n";
		json += "      \"false_positives\": [";
		for (size_t j = 0; j < r.false_positives.size(); ++j) {
			if (j)
				json += ", ";
			json += "\"" + r.false_positives[j] + "\"";
		}
		json += "],\n";
		json += "      \"false_negatives\": [";
		for (size_t j = 0; j < r.false_negatives.size(); ++j) {
			if (j)
				json += ", ";
			json += "\"" + r.false_negatives[j] + "\"";
		}
		json += "]\n";
		json += "    }";
		if (i + 1 < results.size())
			json += ",";
		json += "\n";
	}
	json += "  ]\n";
	json += "}\n";

	fprintf(stderr, "%s", json.c_str());

	FILE *out = fopen("/tmp/codescope_accuracy_report.json", "w");
	if (out) {
		fputs(json.c_str(), out);
		fclose(out);
	}

	// Gate: nonzero if any FP or FN (or a fixture error).
	bool gate_pass = (overall_fp == 0) && (overall_fn == 0) && !had_error &&
			 !results.empty();
	if (gate_pass) {
		fprintf(stderr,
			"\n=== accuracy gate PASSED (0 FP, 0 FN) ===\n");
		return 0;
	}
	fprintf(stderr,
		"\n=== accuracy gate FAILED: fp=%d fn=%d "
		"error=%d ===\n",
		overall_fp, overall_fn, had_error ? 1 : 0);
	return 1;
}
