#include "capability.h"
#include <cstdio>
#include <sqlite3.h>

namespace model
{

CapabilityPlugin::CapabilityPlugin(store::GraphStore *store)
	: store_(store)
{
}

ModelResult CapabilityPlugin::build(uint64_t project_id)
{
	ModelResult r;
	r.plugin_name = "Capability";

	// Extract capabilities from document table (type=0 = README).
	// Each capability is a line in the README that matches capability patterns.
	const char *sql = "SELECT d.file_path, d.content FROM document d "
			  "WHERE d.project_id = ? AND d.type = 0 "
			  "AND d.content != '' LIMIT 5";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(store_->handle(), sql, -1, &stmt, nullptr) !=
	    SQLITE_OK) {
		r.error = "prepare document query failed";
		return r;
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));

	int64_t caps = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *fp = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *content = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		if (!fp || !content)
			continue;

		// Simple capability extraction: look for lines with
		// "support", "capability", "feature", "provides"
		std::string text(content);
		size_t pos = 0;
		while ((pos = text.find_first_of("-\n*", pos)) !=
		       std::string::npos) {
			size_t end = text.find('\n', pos + 1);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			// Check if line looks like a capability
			if (line.find("support") != std::string::npos ||
			    line.find("Supports") != std::string::npos ||
			    line.find("Feature") != std::string::npos ||
			    line.find("feature") != std::string::npos) {
				// Insert capability
				store_->insertCapability(project_id, line, fp,
							 "readme", fp);
				caps++;
			}
			pos = end + 1;
		}
	}
	sqlite3_finalize(stmt);

	r.items_created = caps;
	return r;
}

} // namespace model