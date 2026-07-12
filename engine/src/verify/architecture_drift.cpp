#include "architecture_drift.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sqlite3.h>
#include <unordered_map>

namespace verify
{

namespace
{

// Layer rank for dependency-direction checks. Lower rank = higher layer.
// Controller(0) > Service(1) > Repository(2) in the call hierarchy.
// A call from a higher-rank layer to a lower-rank layer is forward (OK).
// A call from a lower-rank layer to a higher-rank layer is reverse (drift).
int layerRank(const std::string &layer)
{
	if (layer == kLayerController)
		return 0;
	if (layer == kLayerService)
		return 1;
	if (layer == kLayerRepository)
		return 2;
	return -1; // unknown
}

// Case-insensitive check whether `name` ends with `suffix`.
bool endsWithCI(const std::string &name, const std::string &suffix)
{
	if (name.size() < suffix.size())
		return false;
	return std::equal(
		suffix.rbegin(), suffix.rend(), name.rbegin(),
		[](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
}

// Case-insensitive check whether `path` contains `segment` as a substring
// (e.g. path contains "/controllers/").
bool containsCI(const std::string &haystack, const std::string &needle)
{
	if (needle.empty())
		return false;
	auto it = std::search(
		haystack.begin(), haystack.end(), needle.begin(), needle.end(),
		[](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
	return it != haystack.end();
}

// Maximum number of violations to report. Keeps the DriftItem vector and
// the JSON output bounded for very large codebases with many cross-layer
// calls. The limit is high enough to surface all real violations in
// typical projects while preventing pathological output sizes.
inline constexpr int kMaxArchViolations = 100;

} // namespace

std::string classifyEntityLayer(const std::string &name,
				const std::string &file_path)
{
	// Controller: name suffix "Controller" or path /controllers/ /api/
	if (endsWithCI(name, "Controller") ||
	    containsCI(file_path, "/controllers/") ||
	    containsCI(file_path, "/api/")) {
		return kLayerController;
	}

	// Service: name suffix "Service" or path /services/
	if (endsWithCI(name, "Service") ||
	    containsCI(file_path, "/services/")) {
		return kLayerService;
	}

	// Repository: name suffix "Repository"/"Repo"/"Store"/"DAO" or
	// path /repository/ /data/
	if (endsWithCI(name, "Repository") || endsWithCI(name, "Repo") ||
	    endsWithCI(name, "Store") || endsWithCI(name, "DAO") ||
	    containsCI(file_path, "/repository/") ||
	    containsCI(file_path, "/data/")) {
		return kLayerRepository;
	}

	return ""; // unclassified
}

std::vector<DriftItem> detectArchitectureDrift(store::GraphStore &store,
					       uint64_t project_id)
{
	std::vector<DriftItem> drifts;
	sqlite3 *db = store.handle();
	if (!db) {
		fprintf(stderr,
			"[module=verify, method=detectArchitectureDrift] "
			"db handle is null\n");
		return drifts;
	}

	// Scan call edges (relation type=1) and join source + target entities
	// to get their names and file paths for layer classification. We fetch
	// all cross-entity call edges and classify in C++ because the layer
	// logic (suffix + path matching) is hard to express in pure SQL.
	//
	// To keep memory bounded, we stream rows and classify on-the-fly,
	// stopping once we hit kMaxArchViolations.
	const char *sql = "SELECT src.name, src.file_path, "
			  "       tgt.name, tgt.file_path "
			  "FROM relation r "
			  "JOIN entity src ON r.source_id = src.id "
			  "JOIN entity tgt ON r.target_id = tgt.id "
			  "WHERE r.project_id=? AND r.type=1 "
			  "  AND r.source_id != r.target_id";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=verify, method=detectArchitectureDrift] "
			"prepare failed: %s\n",
			sqlite3_errmsg(db));
		return drifts;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	int rc;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *src_name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *src_path = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		const char *tgt_name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 2));
		const char *tgt_path = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 3));

		std::string src_layer = classifyEntityLayer(
			src_name ? src_name : "", src_path ? src_path : "");
		std::string tgt_layer = classifyEntityLayer(
			tgt_name ? tgt_name : "", tgt_path ? tgt_path : "");

		// Skip edges where either endpoint is unclassified — we cannot
		// reason about layer direction for unknown layers.
		if (src_layer.empty() || tgt_layer.empty())
			continue;

		int src_rank = layerRank(src_layer);
		int tgt_rank = layerRank(tgt_layer);

		bool is_violation = false;
		std::string violation_kind;

		if (src_rank > tgt_rank) {
			// Reverse call: lower layer calling a higher layer.
			is_violation = true;
			violation_kind = "reverse call";
		} else if (src_rank == tgt_rank &&
			   src_layer == kLayerController) {
			// Same-layer bypass: Controller calling another Controller
			// directly instead of going through a Service.
			is_violation = true;
			violation_kind = "same-layer bypass";
		}

		if (!is_violation)
			continue;

		DriftItem item;
		item.type = "ArchitectureDrift";
		item.severity = kDriftSeverityArch;
		item.subject = src_layer + "->" + tgt_layer;
		item.detail = std::string(src_name ? src_name : "") + " (" +
			      src_layer + ") calls " +
			      (tgt_name ? tgt_name : "") + " (" + tgt_layer +
			      ") - " + violation_kind;
		drifts.push_back(item);

		if (static_cast<int>(drifts.size()) >= kMaxArchViolations)
			break;
	}
	if (rc != SQLITE_DONE &&
	    static_cast<int>(drifts.size()) < kMaxArchViolations)
		fprintf(stderr,
			"[module=verify, method=detectArchitectureDrift] "
			"step ended with rc=%d: %s\n",
			rc, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);

	return drifts;
}

} // namespace verify
