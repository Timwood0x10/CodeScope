// test_capability_plugin_lines: the capability pass must take its names from
// whole LINES, and must not duplicate what is already stored.
//
// Regression for two defects in CapabilityPlugin::build + insertCapability:
//   * the line scan advanced on any '-' or '*' as well as '\n', so it restarted
//     mid-line and named a capability after the fragment that follows the
//     hyphen ("FastLookup" from "A thread-safe design - Supports fast lookup").
//     That name matches no code, so the capability-drift detector reported it as
//     undocumented — drift invented by the extractor.
//   * the table had no uniqueness rule and the INSERT was unguarded, so every
//     index run appended another copy of every capability, double-counting in
//     capability_state and in the drift output.

#include "../src/model/plugin.h"
#include "../src/model/plugins/capability.h"
#include "../src/store/store.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

static const char *kDbPath = "/tmp/test_capability_plugin_lines.db";

/// Capability names stored for this project, in id order.
static std::vector<std::string> capabilityNames(store::GraphStore &store,
						uint64_t project_id)
{
	std::vector<std::string> names;
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT name FROM capability WHERE project_id=? ORDER BY id";
	assert(sqlite3_prepare_v2(store.handle(), sql, -1, &stmt, nullptr) ==
	       SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *n = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		names.push_back(n ? n : "");
	}
	sqlite3_finalize(stmt);
	return names;
}

int main()
{
	unlink(kDbPath);

	store::GraphStore store;
	if (!store.open(kDbPath)) {
		fprintf(stderr, "FAIL: cannot open store: %s\n",
			store.error().c_str());
		return 1;
	}
	uint64_t pid = store.createProject("/tmp", "test_capability_lines");
	assert(pid > 0);

	model::ModelContext ctx;
	model::DocumentInfo doc;
	doc.file_path = "README.md";
	// Line 1 has a hyphen in the middle: the old scan restarted there.
	// Line 2 is a plain bullet.
	doc.content = "# Tool\n"
		      "A thread-safe design - Supports fast lookup\n"
		      "- Supports incremental indexing\n";
	ctx.documents.push_back(doc);

	model::CapabilityPlugin plugin(&store);
	model::ModelResult r = plugin.build(pid, ctx);
	assert(r.ok());
	assert(r.items_created == 2);

	std::vector<std::string> names = capabilityNames(store, pid);
	assert(names.size() == 2);
	for (const auto &n : names)
		printf("  capability: %s\n", n.c_str());

	// The first name comes from the whole line, not from the fragment after the
	// hyphen. ("FastLookup" was the old, fragment-derived name.)
	assert(names[0].find("ThreadSafe") != std::string::npos);
	assert(names[0] != "FastLookup");
	// The bullet's leading "- " is still stripped.
	assert(names[1] == "IncrementalIndexing");

	// Idempotent: a second pass over the same documents adds nothing.
	model::ModelResult again = plugin.build(pid, ctx);
	assert(again.ok());
	names = capabilityNames(store, pid);
	assert(names.size() == 2);
	printf("  second pass kept %zu capabilities (no duplicates)\n",
	       names.size());

	store.close();
	unlink(kDbPath);
	printf("=== test_capability_plugin_lines PASSED ===\n");
	return 0;
}
