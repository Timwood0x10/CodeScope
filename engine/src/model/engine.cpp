#include "engine.h"
#include <cstdio>
#include <sqlite3.h>

namespace model
{

void ModelEngine::addPlugin(std::unique_ptr<ModelPlugin> plugin)
{
	if (plugin)
		plugins_.push_back(std::move(plugin));
}

bool ModelEngine::populateModelContext(uint64_t project_id, ModelContext &ctx)
{
	if (!store_) {
		fprintf(stderr, "[module=model, method=populateModelContext] "
				"store is null\n");
		return false;
	}
	sqlite3 *db = store_->handle();
	if (!db) {
		fprintf(stderr, "[module=model, method=populateModelContext] "
				"db handle is null\n");
		return false;
	}

	const int64_t pid = static_cast<int64_t>(project_id);

	// ── Entities ──────────────────────────────────────────────────
	// One scan of the entity table replaces the per-plugin SELECTs
	// previously issued by Workflow / Capability / Contract plugins.
	{
		const char *sql = "SELECT id, name, file_path, qualified_name, "
				  "module_path, kind, project_id "
				  "FROM entity WHERE project_id = ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=model, method=populateModelContext] "
				"prepare entities failed: %s\n",
				sqlite3_errmsg(db));
			return false;
		}
		if (sqlite3_bind_int64(st, 1, pid) != SQLITE_OK) {
		    fprintf(stderr,
		     "[module=model, method=populateModelContext] "
		     "bind entities failed: %s\n",
		     sqlite3_errmsg(db));
		    sqlite3_finalize(st);
		    return false;
		   }
		   while (sqlite3_step(st) == SQLITE_ROW) {
		    EntityInfo e;
		    e.id = static_cast<uint64_t>(
		     sqlite3_column_int64(st, 0));
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 1));
			e.name = s ? s : "";
			s = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 2));
			e.file_path = s ? s : "";
			s = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 3));
			e.qualified_name = s ? s : "";
			s = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 4));
			e.module_path = s ? s : "";
			e.kind = sqlite3_column_int(st, 5);
			e.project_id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 6));
			ctx.entity_ids_ordered.push_back(e.id);
			ctx.entities_by_id.emplace(e.id, std::move(e));
		}
		sqlite3_finalize(st);
	}

	// ── Relations ─────────────────────────────────────────────────
	// One scan populates both all_relations and the call_edges subset
	// (type == kRelationTypeCall). Row order follows relation.id
	// (SQLite natural scan order) so plugins that rely on callee
	// ordering match the original LIMIT-based queries.
	{
		const char *sql = "SELECT id, source_id, target_id, type, "
				  "project_id FROM relation "
				  "WHERE project_id = ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=model, method=populateModelContext] "
				"prepare relations failed: %s\n",
				sqlite3_errmsg(db));
			return false;
		}
		if (sqlite3_bind_int64(st, 1, pid) != SQLITE_OK) {
		    fprintf(stderr,
		     "[module=model, method=populateModelContext] "
		     "bind relations failed: %s\n",
		     sqlite3_errmsg(db));
		    sqlite3_finalize(st);
		    return false;
		   }
		   while (sqlite3_step(st) == SQLITE_ROW) {
		    RelationRow r;
		    r.id = static_cast<uint64_t>(
		     sqlite3_column_int64(st, 0));
			r.source_id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 1));
			r.target_id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 2));
			r.type = sqlite3_column_int(st, 3);
			r.project_id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 4));
			ctx.all_relations.push_back(r);
			if (r.type == kRelationTypeCall)
				ctx.call_edges.push_back(r);
		}
		sqlite3_finalize(st);
	}

	// ── Module scopes (kind == 1) ─────────────────────────────────
	// Used by ArchitecturePlugin for cross-module edge detection.
	{
		const char *sql = "SELECT id, name, kind, project_id "
				  "FROM scope WHERE project_id = ? "
				  "AND kind = ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=model, method=populateModelContext] "
				"prepare scopes failed: %s\n",
				sqlite3_errmsg(db));
			return false;
		}
		if (sqlite3_bind_int64(st, 1, pid) != SQLITE_OK ||
		       sqlite3_bind_int(st, 2, kScopeKindModule) != SQLITE_OK) {
		    fprintf(stderr,
		     "[module=model, method=populateModelContext] "
		     "bind scopes failed: %s\n",
		     sqlite3_errmsg(db));
		    sqlite3_finalize(st);
		    return false;
		   }
		   while (sqlite3_step(st) == SQLITE_ROW) {
			ScopeInfo sc;
			sc.id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 0));
			const char *s = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 1));
			sc.name = s ? s : "";
			sc.kind = sqlite3_column_int(st, 2);
			sc.project_id = static_cast<uint64_t>(
				sqlite3_column_int64(st, 3));
			ctx.scope_modules.push_back(std::move(sc));
		}
		sqlite3_finalize(st);
	}

	// ── README documents (type == 0) ──────────────────────────────
	// Used by CapabilityPlugin and ContractPlugin for keyword mining.
	{
		const char *sql = "SELECT file_path, content FROM document "
				  "WHERE project_id = ? AND type = ? "
				  "AND content != '' LIMIT ?";
		sqlite3_stmt *st = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=model, method=populateModelContext] "
				"prepare documents failed: %s\n",
				sqlite3_errmsg(db));
			return false;
		}
		if (sqlite3_bind_int64(st, 1, pid) != SQLITE_OK ||
		       sqlite3_bind_int(st, 2, kDocumentTypeReadme) != SQLITE_OK ||
		       sqlite3_bind_int(st, 3, kMaxReadmeDocuments) != SQLITE_OK) {
		    fprintf(stderr,
		     "[module=model, method=populateModelContext] "
		     "bind documents failed: %s\n",
		     sqlite3_errmsg(db));
		    sqlite3_finalize(st);
		    return false;
		   }
		   while (sqlite3_step(st) == SQLITE_ROW) {
			DocumentInfo d;
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 0));
			d.file_path = fp ? fp : "";
			const char *ct = reinterpret_cast<const char *>(
				sqlite3_column_text(st, 1));
			d.content = ct ? ct : "";
			ctx.documents.push_back(std::move(d));
		}
		sqlite3_finalize(st);
	}

	return true;
}

int64_t ModelEngine::runAll(uint64_t project_id)
{
	if (!store_) {
		fprintf(stderr,
			"[module=model, method=runAll] store is null\n");
		return 0;
	}

	// Populate the shared ModelContext once — this replaces the N
	// sequential entity/relation scans previously performed by each
	// plugin individually.
	ModelContext ctx;
	if (!populateModelContext(project_id, ctx)) {
		fprintf(stderr, "[module=model, method=runAll] "
				"failed to populate ModelContext\n");
		return 0;
	}

	// Wrap all plugin INSERTs in a single transaction so SQLite batches
	// the commit into one fsync instead of one per plugin.
	if (!store_->beginTransaction()) {
		fprintf(stderr,
			"[module=model, method=runAll] "
			"BEGIN failed: %s\n",
			store_->error().c_str());
		return 0;
	}

	int64_t total = 0;
	for (auto &p : plugins_) {
		if (!p)
			continue;
		auto result = p->build(project_id, ctx);
		if (result.ok()) {
			total += result.items_created;
			fprintf(stderr, "[model] %s: created %lld items\n",
				p->name(), (long long)result.items_created);
		} else {
			fprintf(stderr, "[model] %s: failed: %s\n", p->name(),
				result.error.c_str());
		}
	}

	if (!store_->commitTransaction()) {
		fprintf(stderr,
			"[module=model, method=runAll] "
			"COMMIT failed: %s\n",
			store_->error().c_str());
		store_->rollbackTransaction();
		return 0;
	}
	return total;
}

ModelResult ModelEngine::run(const std::string &name, uint64_t project_id)
{
	// Populate a context for single-plugin runs as well, so that the
	// plugin interface is uniform.
	ModelContext ctx;
	if (!populateModelContext(project_id, ctx)) {
		ModelResult r;
		r.plugin_name = name;
		r.error = "failed to populate ModelContext";
		return r;
	}
	for (auto &p : plugins_) {
		if (p && p->name() == name)
			return p->build(project_id, ctx);
	}
	ModelResult r;
	r.plugin_name = name;
	r.error = "plugin not found: " + name;
	return r;
}

std::vector<const char *> ModelEngine::pluginNames() const
{
	std::vector<const char *> names;
	for (auto &p : plugins_) {
		if (p)
			names.push_back(p->name());
	}
	return names;
}

} // namespace model
