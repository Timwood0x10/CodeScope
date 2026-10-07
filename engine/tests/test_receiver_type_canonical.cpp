// test_receiver_type_canonical — one type, one spelling, every lookup.
//
// Defect guarded: a type name reaches the resolver in the spelling of its use
// site, while the tables it is looked up in (global_struct_fields_,
// global_var_types_, interface_impl_index_) are keyed by the bare name the
// declaration recorded. The field-chain walk (Step 8.1c) looked the RAW
// spelling up, so a parameter recorded as `*HolderA` never reached the entry
// keyed `HolderA`: the chain was abandoned and the walk moved on to the next
// same-named variable's type. On goagent that is how six `dispatch` call sites
// (`s.agents.Get(id)`) resolved through whichever same-named variable happened
// to be tried first, and why the result followed the module packing; in a
// fixture where the pointer spelling was the ONLY candidate the walk produced
// no edge at all while the value spelling produced two. The receiver-match
// factor had the same hole from the other side, building `prefix1` as
// `*Manager::`, a string no candidate's qualified_name can contain.
//
// Part 1 pins canonicalTypeName's boundaries (empty, sigil-only, pointers,
// references, package qualifiers, whitespace, trailing sigils, a Go slice).
// Part 2 drives the real index path over a Go fixture whose call sites can only
// resolve through that walk, in the three spellings that matter: a pointer
// parameter, a pointer to a struct whose FIELD is the pointer, and a type no
// declaration mentions (which must stay unresolved rather than guessed).
#include "../include/engine.h"
#include "../src/resolver/factors.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

#include "test_check.h"
#include "test_engine_handle.h"

namespace
{

/// Assert one canonicalisation, naming both sides on failure.
void expectCanonical(const char *in, const char *want)
{
	const std::string got = resolver::canonicalTypeName(in);
	if (got != want) {
		fprintf(stderr, "FAIL: canonicalTypeName(\"%s\") = \"%s\", want \"%s\"\n",
			in, got.c_str(), want);
	}
	CHECK(got == want);
}

/// Write `content` to `path`, failing the test if the file cannot be created.
void writeFile(const std::string &path, const char *content)
{
	FILE *f = fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	fputs(content, f);
	fclose(f);
}

/// One scalar read.
long long scalarSql(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	long long value = -1;
	if (sqlite3_step(st) == SQLITE_ROW)
		value = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return value;
}

/// The resolution kind of the edge `caller -> callee`, or "(no edge)".
std::string edgeKind(sqlite3 *db, const char *caller, const char *callee)
{
	const char *sql =
		"SELECT r.resolution_kind FROM relation r "
		"JOIN entity se ON se.id = r.source_id "
		"JOIN entity te ON te.id = r.target_id "
		"WHERE r.type = 1 AND se.name = ? AND te.name = ? "
		"ORDER BY r.resolution_kind LIMIT 1";
	sqlite3_stmt *st = nullptr;
	CHECK(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
	sqlite3_bind_text(st, 1, caller, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, callee, -1, SQLITE_TRANSIENT);
	std::string out = "(no edge)";
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		if (s)
			out = s;
	}
	sqlite3_finalize(st);
	return out;
}

} // namespace

int main()
{
	// ── Part 1: canonicalTypeName boundaries ──────────────────────
	expectCanonical("", "");
	expectCanonical("*", "");
	expectCanonical("&", "");
	expectCanonical("***", "");
	expectCanonical("PluginBus", "PluginBus");
	expectCanonical("plugin_bus", "plugin_bus");
	expectCanonical("*PluginBus", "PluginBus");
	expectCanonical("&PluginBus", "PluginBus");
	expectCanonical("**PluginBus", "PluginBus");
	expectCanonical("PluginBus*", "PluginBus"); // pointer-suffix spelling
	expectCanonical("PluginBus&", "PluginBus");
	expectCanonical("ares_runtime.PluginBus", "PluginBus");
	expectCanonical("*ares_runtime.PluginBus", "PluginBus");
	expectCanonical("a.b.C", "C");
	expectCanonical(".Leading", "Leading");
	expectCanonical(" *\tPluginBus ", "PluginBus");
	expectCanonical("ares_runtime::PluginBus", "PluginBus");
	expectCanonical("std::vector<int>", "vector");
	expectCanonical("Holder[T]", "Holder");
	expectCanonical("pkg.Holder[T]", "Holder");
	expectCanonical("std::map<K, V>", "map");
	// Composite spellings carry punctuation a type name cannot contain, and they
	// are what Go records for channels, slices, maps and function types. Mining
	// them for a last segment is how `<-chan os.Signal` becomes `Signal` — a
	// real symbol in most code bases — so they are reported as unknown instead.
	expectCanonical("[]byte", "");
	expectCanonical("[]<-chan *Event", "");
	expectCanonical("map[string]int", "");
	expectCanonical("<-chan os.Signal", "");
	expectCanonical("chan<- *Event", "");
	expectCanonical("func(ctx context.Context, s string) (<-chan T, error)", "");
	expectCanonical("::", "");

	// ── Part 2: the field-chain walk through the real index path ──
	const std::string proj_dir = "/tmp/receiver_type_canonical_repro";
	std::filesystem::remove_all(proj_dir);
	std::filesystem::create_directories(proj_dir + "/src");

	writeFile(proj_dir + "/src/iface.go",
		  "package p\n"
		  "\n"
		  "type Getter interface {\n"
		  "\tGet(id string) error\n"
		  "}\n"
		  "\n"
		  "type Registry struct{}\n"
		  "\n"
		  "func (r *Registry) Get(id string) error { return nil }\n");
	writeFile(proj_dir + "/src/holders.go",
		  "package p\n"
		  "\n"
		  "type Holder struct {\n"
		  "\tregistry Getter\n"
		  "}\n"
		  "\n"
		  "type Outer struct {\n"
		  "\tinner *Holder\n"
		  "}\n");
	writeFile(proj_dir + "/src/use_ptr.go",
		  "package p\n"
		  "\n"
		  "func usePtr(h *Holder, id string) error {\n"
		  "\treturn h.registry.Get(id)\n"
		  "}\n");
	writeFile(proj_dir + "/src/use_nested.go",
		  "package p\n"
		  "\n"
		  "func useNested(o *Outer, id string) error {\n"
		  "\treturn o.inner.registry.Get(id)\n"
		  "}\n");
	writeFile(proj_dir + "/src/use_unknown.go",
		  "package p\n"
		  "\n"
		  "func useUnknown(u Undeclared, id string) error {\n"
		  "\treturn u.registry.Get(id)\n"
		  "}\n");

	const std::string db_path = proj_dir + "/canonical.db";
	unlink(db_path.c_str());
	unlink((db_path + "-wal").c_str());
	unlink((db_path + "-shm").c_str());

	g_engine = engine_create(db_path.c_str());
	CHECK(g_engine != nullptr);
	const uint64_t pid = engine_create_project(g_engine, proj_dir.c_str(),
						   "receiver-canonical");
	CHECK(pid > 0);

	const std::string request = "{\"paths\":[\"" + proj_dir +
				    "/src/iface.go\",\"" + proj_dir +
				    "/src/holders.go\",\"" + proj_dir +
				    "/src/use_ptr.go\",\"" + proj_dir +
				    "/src/use_nested.go\",\"" + proj_dir +
				    "/src/use_unknown.go\"]}";
	char *res = engine_index_files(g_engine, pid, request.c_str(), 1);
	CHECK(res != nullptr);
	CHECK(strstr(res, "\"ok\":true") != nullptr);
	engine_free_string(res);
	// No wait needed before reading the database directly: engine_destroy()
	// joins the async knowledge builder (engine_lifecycle.cpp) before it closes
	// the store, and everything asserted below — relation, entity,
	// semantic_records — is written synchronously by the index path, not by the
	// builder. A fixed sleep here would only add a race against slow CI.
	engine_destroy(g_engine);
	g_engine = nullptr;

	sqlite3 *db = nullptr;
	CHECK(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);

	// The call sites below are only meaningful while the name is ambiguous —
	// the interface method and its implementation are both called `Get` — and
	// while the fixture really parsed (the three call sites exist).
	CHECK(scalarSql(db, "SELECT COUNT(*) FROM entity WHERE name='Get'") >= 2);
	CHECK(scalarSql(db, "SELECT COUNT(*) FROM entity WHERE name IN "
			    "('usePtr','useNested','useUnknown')") == 3);

	// `h *Holder`: the chain is `h` → *Holder → registry → Getter, so every
	// lookup depends on stripping the pointer sigil before hitting a table.
	CHECK(edgeKind(db, "usePtr", "Get") == "dispatch");
	// `o *Outer.inner` → *Holder → registry → Getter: the sigil appears twice,
	// once per segment.
	CHECK(edgeKind(db, "useNested", "Get") == "dispatch");
	// `u Undeclared` resolves to nothing: no table has that struct, so the walk
	// must abstain instead of falling through to another variable's type.
	CHECK(edgeKind(db, "useUnknown", "Get") == "(no edge)");

	sqlite3_close(db);
	unlink(db_path.c_str());
	if (checkFailures()) {
		fprintf(stderr, "=== receiver type canonicalisation: FAILED ===\n");
		return 1;
	}
	printf("=== receiver type canonicalisation test passed ===\n");
	return 0;
}
