#include "pipeline.h"
#include <cctype>
#include <cstdio>
#include <sqlite3.h>
#include <unordered_set>

namespace resolver
{

namespace
{
// Infer the source language from a file path's extension. Mirrors the
// helper in pipeline.cpp (kept as a per-TU copy because it lives in an
// anonymous namespace there; ODR-safe since anonymous namespaces isolate
// each translation unit).
std::string languageFromPath(const std::string &file_path)
{
	size_t dot = file_path.rfind('.');
	if (dot == std::string::npos)
		return "";
	std::string ext = file_path.substr(dot);
	std::string lower;
	lower.reserve(ext.size());
	for (char ch : ext)
		lower.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch))));
	if (lower == ".cpp" || lower == ".cc" || lower == ".cxx" ||
	    lower == ".c" || lower == ".h" || lower == ".hpp" ||
	    lower == ".hh" || lower == ".hxx")
		return "cpp";
	if (lower == ".rs")
		return "rust";
	if (lower == ".py")
		return "python";
	if (lower == ".go")
		return "go";
	if (lower == ".ts" || lower == ".tsx")
		return "typescript";
	if (lower == ".js" || lower == ".jsx")
		return "javascript";
	if (lower == ".java")
		return "java";
	return "";
}
} // namespace

int ResolverPipeline::loadEntityIndex(
	std::unordered_map<std::string, std::vector<Candidate>> &entity_index,
	std::unordered_map<uint64_t, const Candidate *> &entity_by_id,
	int64_t &total_entities)
{
	// ── Step 0: Pre-load all entities into a name-indexed HashMap ──
	// This avoids one SQL query per reference (the main bottleneck).
	{
		std::string idx_sql =
			// Include arity so factorSignatureMatch can distinguish
			// same-name overloads (init()/init(int)/init(string)).
			// entity.arity was added in v0.5+ migration (store_schema.cpp:470).
			// Without this column in the SELECT, c.arity defaulted to 0
			// and every candidate scored kScorePartialMatch (0.5),
			// letting std::sort pick the winner by unstable order.
			// See CODE_REVIEW_FINDINGS_2026-07-19.md C2.
			// Include kind (appended as column 5) so
			// factorConstructorMatch can prefer Class/Struct targets;
			// previously kind was hardcoded 0 in the call, so the
			// constructor factor always returned 0.0 (M-11).
			// Step 5: include qualified_name (column 6) so
			// factorReceiverTypeMatch can match "Box::draw" against
			// receiver_type="Box" instead of using directory heuristics.
			"SELECT id, name, file_path, language, arity, kind, qualified_name "
			"FROM entity "
			"WHERE project_id=? AND name != ''";
		sqlite3_stmt *idx_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), idx_sql.c_str(), -1,
				       &idx_st, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"[module=resolver, method=run] "
				"prepare entity index failed: %s\n",
				sqlite3_errmsg(store_->handle()));
			return -1;
		}
		sqlite3_bind_int64(idx_st, 1,
				   static_cast<int64_t>(project_id_));
		while (sqlite3_step(idx_st) == SQLITE_ROW) {
			Candidate c;
			c.entity_id = static_cast<uint64_t>(
				sqlite3_column_int64(idx_st, 0));
			const char *n = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 1));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 2));
			const char *lang = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 3));
			c.name = n ? n : "";
			c.file_path = fp ? fp : "";
			c.language = lang ? lang :
					    languageFromPath(c.file_path);
			c.module_path = modulePath(c.file_path);
			// v0.6 (perf): precompute the path components that
			// applyConstraints derives from c.file_path on every candidate.
			// Computing them once here (dir/parent/module token) removes
			// per-ref heap allocations in the hot loop; the values are
			// byte-identical to the old inline rfind+substr derivation so no
			// score changes.
			{
				size_t cs = c.file_path.rfind('/');
				c.cand_dir = (cs != std::string::npos) ?
						     c.file_path.substr(0, cs) :
						     std::string();
				size_t cps = c.cand_dir.rfind('/');
				c.cand_parent =
					(cps != std::string::npos) ?
						c.cand_dir.substr(0, cps) :
						std::string();
				c.cand_module = c.cand_dir;
				size_t cms = c.cand_dir.rfind('/');
				if (cms != std::string::npos)
					c.cand_module =
						c.cand_dir.substr(cms + 1);
			}
			// Column 4 is arity (added to SELECT above). Default 0
			// if NULL — matches entity.arity DEFAULT 0 so callers
			// that never set arity behave as "unknown arity".
			c.arity = sqlite3_column_int(idx_st, 4);
			// Column 5 is kind (RecordKind). Default 0 if NULL —
			// matches entity.kind NOT NULL semantics; 0 = Function,
			// so non-type candidates correctly score 0.0 on the
			// constructor factor.
			c.kind = sqlite3_column_int(idx_st, 5);
			// Step 5: column 6 is qualified_name. Used by
			// factorReceiverTypeMatch to match "Box::draw" against
			// receiver_type="Box". Empty for languages that don't
			// populate it (e.g. Go), causing the factor to fall back
			// to file-path matching.
			const char *qn = reinterpret_cast<const char *>(
				sqlite3_column_text(idx_st, 6));
			c.qualified_name = qn ? qn : "";
			c.score = 0;
			entity_index[c.name].push_back(c);
			total_entities++;
		}
		sqlite3_finalize(idx_st);
	}

	// Build an id -> Candidate map so fuzzy-resolved entity ids can be
	// hydrated into full candidates without a per-id SQL lookup. The
	// entity_index above already carries every field the old lk_sql
	// lookup returned (name, file_path, language, arity, kind,
	// qualified_name) plus precomputed path components; copying from it
	// is byte-identical to the previous SQL materialization.
	entity_by_id.reserve(static_cast<size_t>(total_entities));
	for (const auto &entry : entity_index) {
		for (const auto &c : entry.second)
			entity_by_id[c.entity_id] = &c;
	}

	// ── Step 0b: Pre-load all imports into a file_path-indexed HashMap ──
	// This is the core fix for the 174s bottleneck: factorImportMatch
	// previously ran `SELECT COUNT(*) FROM import WHERE project_id=? AND
	// file_path=? AND target_path LIKE '%module_name%'` per candidate.
	// The leading-% LIKE is non-sargable, forcing a FULL TABLE SCAN on
	// the import table for each of the ~313k candidate evaluations
	// (108k refs × 2.9 avg candidates × 1-2 SQL queries each).
	//
	// We load every import row once into import_index_ (file_path ->
	// list of target_path strings) and let factorImportMatch match
	// in-memory with SQLite-exact LIKE semantics. Resolved edges are
	// IDENTICAL to the SQL implementation; only the access path changes.
	import_index_.clear();
	{
		std::string imp_sql =
			"SELECT file_path, target_path FROM import "
			"WHERE project_id=?";
		sqlite3_stmt *imp_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), imp_sql.c_str(), -1,
				       &imp_st, nullptr) != SQLITE_OK) {
			fprintf(stderr,
				"[module=resolver, method=run] "
				"prepare import index failed: %s\n",
				sqlite3_errmsg(store_->handle()));
			return -1;
		}
		sqlite3_bind_int64(imp_st, 1,
				   static_cast<int64_t>(project_id_));
		while (sqlite3_step(imp_st) == SQLITE_ROW) {
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(imp_st, 0));
			const char *tp = reinterpret_cast<const char *>(
				sqlite3_column_text(imp_st, 1));
			std::string file_path = fp ? fp : "";
			std::string target_path = tp ? tp : "";
			// NOTE: empty file_path rows are intentionally KEPT. The
			// original SQL used an exact `file_path = ?` predicate,
			// which matches empty-file_path rows when caller_file is
			// also empty; dropping them would change matching results
			// for such (rare) call sites and break identical-edge
			// semantics. They land in the "" bucket and are only
			// consulted when a caller's file_path is empty.
			import_index_[file_path].push_back(
				std::move(target_path));
		}
		sqlite3_finalize(imp_st);
	}
	return 0;
}

void ResolverPipeline::loadDispatchIndex()
{
	// ── Step 8 (plan §8.1): Pre-load interface/trait implementations ──
	// InterfaceImpl records (kind=20) store (name=implementing_type,
	// type_name=interface_name). We build a map from interface name
	// to all implementing types, so the hot loop can expand
	// Interface/Virtual dispatch calls into bounded candidate sets.
	interface_impl_index_.clear();
	int64_t total_iface_impls = 0;
	{
		// semantic_records.kind=20 is InterfaceImpl. The `name` column
		// holds the implementing type, `type_name` holds the interface.
		std::string iface_sql =
			"SELECT name, type_name FROM semantic_records "
			"WHERE project_id=? AND kind=20 AND name != '' "
			"AND type_name != ''";
		sqlite3_stmt *iface_st = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), iface_sql.c_str(), -1,
				       &iface_st, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(iface_st, 1,
					   static_cast<int64_t>(project_id_));
			while (sqlite3_step(iface_st) == SQLITE_ROW) {
				const char *impl =
					reinterpret_cast<const char *>(
						sqlite3_column_text(iface_st,
								    0));
				const char *iface =
					reinterpret_cast<const char *>(
						sqlite3_column_text(iface_st,
								    1));
				if (impl && iface)
					interface_impl_index_[iface].push_back(
						impl);
				total_iface_impls++;
			}
			sqlite3_finalize(iface_st);
		}
	}

	// ── Step 8 (plan §8.1b): cross-file interface method-set matching ──
	// The visitor's kind=20 records only cover same-file (struct,
	// interface) pairs — Go interfaces are usually declared in one file
	// and implemented in another, so those never match in-file. This
	// global pass reconstructs method sets from the per-method qualified
	// names ("Struct.method" set by handleMethodDecl, "Interface.method"
	// set by handleInterfaceMethod) and re-runs the subset check across
	// ALL files, supplementing interface_impl_index_ with cross-file
	// implementations.
	{
		// Interface entity names (kind=3) — used to classify a method's
		// qualified-name prefix as an interface vs a struct.
		std::unordered_set<std::string> iface_names;
		{
			std::string names_sql =
				"SELECT name FROM semantic_records "
				"WHERE project_id=? AND kind=3 AND name != ''";
			sqlite3_stmt *nst = nullptr;
			if (sqlite3_prepare_v2(store_->handle(),
					       names_sql.c_str(), -1, &nst,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					nst, 1,
					static_cast<int64_t>(project_id_));
				while (sqlite3_step(nst) == SQLITE_ROW) {
					const char *n =
						reinterpret_cast<const char *>(
							sqlite3_column_text(nst,
									    0));
					if (n)
						iface_names.insert(n);
				}
				sqlite3_finalize(nst);
			}
		}
		// Method records with qualified names — split "Type.method".
		std::unordered_map<std::string, std::vector<std::string>>
			iface_methods; // interface name -> its methods
		std::unordered_map<std::string, std::vector<std::string>>
			struct_methods; // struct type -> its methods
		{
			std::string meth_sql =
				"SELECT qualified_name FROM semantic_records "
				"WHERE project_id=? AND kind=1 AND "
				"qualified_name != ''";
			sqlite3_stmt *mst = nullptr;
			if (sqlite3_prepare_v2(store_->handle(),
					       meth_sql.c_str(), -1, &mst,
					       nullptr) == SQLITE_OK) {
				sqlite3_bind_int64(
					mst, 1,
					static_cast<int64_t>(project_id_));
				while (sqlite3_step(mst) == SQLITE_ROW) {
					const char *qn =
						reinterpret_cast<const char *>(
							sqlite3_column_text(mst,
									    0));
					if (!qn)
						continue;
					std::string q(qn);
					size_t dot = q.find('.');
					if (dot == std::string::npos)
						continue;
					std::string type_name =
						q.substr(0, dot);
					std::string method = q.substr(dot + 1);
					if (type_name.empty() || method.empty())
						continue;
					if (iface_names.count(type_name) > 0)
						iface_methods[type_name]
							.push_back(method);
					else
						struct_methods[type_name]
							.push_back(method);
				}
				sqlite3_finalize(mst);
			}
		}
		// Global subset check: struct implements interface iff the
		// struct's method set contains every interface method.
		// v0.2.5 (perf fix): pre-index each struct's method set into a
		// hash set once, then the interface-implements check is O(1) per
		// method instead of a linear std::find. Without this, the
		// for-interface × for-struct × for-method triple loop was O(I×S×M)
		// — quadratic and noticeable on large Go projects with many
		// interfaces/structs.
		std::unordered_map<std::string, std::unordered_set<std::string>>
			struct_method_set;
		struct_method_set.reserve(struct_methods.size());
		for (const auto &sentry : struct_methods) {
			auto &s = struct_method_set[sentry.first];
			s.reserve(sentry.second.size());
			s.insert(sentry.second.begin(), sentry.second.end());
		}
		for (const auto &iface_entry : iface_methods) {
			const std::string &iface = iface_entry.first;
			const auto &imethods = iface_entry.second;
			if (imethods.empty())
				continue;
			for (const auto &sentry : struct_methods) {
				const std::string &stype = sentry.first;
				if (stype == iface)
					continue;
				auto smit = struct_method_set.find(stype);
				if (smit == struct_method_set.end())
					continue;
				const auto &smethods = smit->second;
				bool implements_all = true;
				for (const auto &m : imethods) {
					if (smethods.find(m) ==
					    smethods.end()) {
						implements_all = false;
						break;
					}
				}
				if (implements_all) {
					// Avoid duplicating an entry the
					// visitor's kind=20 pass already added.
					auto &impls =
						interface_impl_index_[iface];
					if (std::find(impls.begin(),
						      impls.end(),
						      stype) == impls.end()) {
						impls.push_back(stype);
						total_iface_impls++;
					}
				}
			}
		}
		if (total_iface_impls > 0) {
			fprintf(stderr,
				"[module=resolver, method=run] interface_impl_index: "
				"%lld implementation(s) loaded (%d interface(s))\n",
				static_cast<long long>(total_iface_impls),
				static_cast<int>(interface_impl_index_.size()));
		}
	}

	// ── Step 8 (plan §8.1c): rebuild the global struct field table ──
	// The Go visitor persists each struct field as a TypeRef record
	// (kind=14) under the struct entity (kind=2): name = field name,
	// type_name = field type. Rebuilding here makes the table complete
	// across files, so field-chain receivers (r.pluginBus.AfterStep)
	// whose receiver_type was empty at visit time (struct declared in
	// another file) can be resolved before dispatch expansion.
	global_struct_fields_.clear();
	{
		std::string field_sql =
			"SELECT p.name, t.name, t.type_name "
			"FROM semantic_records t "
			"JOIN semantic_records p ON t.parent_id = p.original_id "
			"AND p.project_id = t.project_id "
			"AND p.file_path = t.file_path "
			"WHERE t.project_id=? AND t.kind=17 AND p.kind=2 "
			"AND t.name != '' AND t.type_name != ''";
		sqlite3_stmt *fst = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), field_sql.c_str(), -1,
				       &fst, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(fst, 1,
					   static_cast<int64_t>(project_id_));
			while (sqlite3_step(fst) == SQLITE_ROW) {
				const char *stype =
					reinterpret_cast<const char *>(
						sqlite3_column_text(fst, 0));
				const char *fname =
					reinterpret_cast<const char *>(
						sqlite3_column_text(fst, 1));
				const char *ftype =
					reinterpret_cast<const char *>(
						sqlite3_column_text(fst, 2));
				if (stype && fname && ftype)
					global_struct_fields_[stype][fname] =
						ftype;
			}
			sqlite3_finalize(fst);
		}
	}

	// ── Step 8.1c (plan §8): global caller variable-type table ──
	// The Go visitor persists method receivers and declared variables
	// as TypeRef records (kind=14) under the containing function/method
	// entity (kind=0/1): name = variable name, type_name = its type.
	// Rebuilding here gives the field-chain resolver the first-segment
	// type ("r" -> "Runner") when resolving "r.pluginBus.AfterStep".
	global_var_types_.clear();
	{
		std::string vtype_sql =
			"SELECT t.name, t.type_name FROM semantic_records t "
			"JOIN semantic_records p ON t.parent_id = p.original_id "
			"AND p.project_id = t.project_id "
			"AND p.file_path = t.file_path "
			"WHERE t.project_id=? AND t.kind=17 "
			"AND p.kind IN (0,1) "
			"AND t.name != '' AND t.type_name != ''";
		sqlite3_stmt *vst = nullptr;
		if (sqlite3_prepare_v2(store_->handle(), vtype_sql.c_str(), -1,
				       &vst, nullptr) == SQLITE_OK) {
			sqlite3_bind_int64(vst, 1,
					   static_cast<int64_t>(project_id_));
			while (sqlite3_step(vst) == SQLITE_ROW) {
				const char *vname =
					reinterpret_cast<const char *>(
						sqlite3_column_text(vst, 0));
				const char *vtype =
					reinterpret_cast<const char *>(
						sqlite3_column_text(vst, 1));
				if (vname && vtype)
					global_var_types_[vname].push_back(
						vtype);
			}
			sqlite3_finalize(vst);
		}
	}
}

} // namespace resolver
