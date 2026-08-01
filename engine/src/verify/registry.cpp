#include "registry.h"
#include "../store/store.h"
#include "architecture_verifier.h"
#include "capability_verifier.h"
#include "contract_verifier.h"
#include "function_implements_verifier.h"

#include <cstdio>
#include <sqlite3.h>
#include <unordered_set>

namespace verify
{

// Meyers singleton: a function-local static is thread-safe under C++11 and
// later, so no extra mutex is needed for initialization.
VerifierRegistry &VerifierRegistry::instance()
{
	static VerifierRegistry registry;
	return registry;
}

void VerifierRegistry::register_verifier(std::unique_ptr<Verifier> v)
{
	verifiers_.push_back(std::move(v));
}

void VerifierRegistry::register_default_verifiers(store::GraphStore *store,
						  uint64_t project_id)
{
	// Register in priority order: capability first (most specific), then
	// contract, then architecture, then function_implements. Each verifier
	// accepts a disjoint ClaimType so the order does not affect
	// correctness, but keeping a predictable order makes debugging
	// dispatch issues easier.
	register_verifier(
		std::make_unique<CapabilityVerifier>(store, project_id));
	register_verifier(
		std::make_unique<ContractVerifier>(store, project_id));
	register_verifier(
		std::make_unique<ArchitectureVerifier>(store, project_id));
	register_verifier(std::make_unique<FunctionImplementsVerifier>(
		store, project_id));
}

// Idempotent registration of sentinel verifiers. Sentinels use nullptr/0
// because their accepts() only inspects claim.type — the actual verify()
// call is dispatched on a freshly-constructed verifier bound to the
// caller's project_id (see makeVerifierForClaim in engine_verify_ffi.cpp).
//
// This replaces the old `static bool initialized` flag in
// engine_verify_ffi.cpp. The flag was the root cause of the lifecycle bug
// (A15): engine_shutdown() cleared the registry but the flag stayed true,
// so the next ensureVerifiersRegistered() was a no-op and the registry
// stayed empty. Checking the actual registry state makes the function
// symmetric with engine_shutdown()'s clear() — repeatable any number of
// times.
void VerifierRegistry::ensureDefaultVerifiers(store::GraphStore *store,
					      uint64_t project_id)
{
	if (!verifiers_.empty())
		return;
	register_default_verifiers(store, project_id);
}

void VerifierRegistry::clear()
{
	verifiers_.clear();
}

Verifier *VerifierRegistry::match(const Claim &claim) const
{
	for (const auto &v : verifiers_) {
		if (v && v->accepts(claim))
			return v.get();
	}
	return nullptr;
}

std::vector<std::string> VerifierRegistry::verifier_names() const
{
	std::vector<std::string> names;
	names.reserve(verifiers_.size());
	for (const auto &v : verifiers_)
		names.push_back(v ? v->name() : std::string());
	return names;
}

std::vector<ClaimType> VerifierRegistry::supported_claim_types() const
{
	std::vector<ClaimType> supported;
	std::unordered_set<uint8_t> seen;
	for (const auto &v : verifiers_) {
		if (!v)
			continue;
		// Probe each public claim type. The loop is O(verifiers *
		// claim_types) which is tiny (4 types, ~4 verifiers).
		for (ClaimType t : all_public_claim_types()) {
			uint8_t key = static_cast<uint8_t>(t);
			if (seen.count(key))
				continue;
			Claim probe;
			probe.type = t;
			if (v->accepts(probe)) {
				supported.push_back(t);
				seen.insert(key);
			}
		}
	}
	return supported;
}

// The public claim types are those advertised in the MCP schema (see
// server/src/tools/mod.rs verify_claim tool description). They map 1:1 to
// the verify::ClaimType enum. Keeping a single canonical list here ensures
// the introspection API and the coverage test agree on what "public"
// means — no claim type can be silently missing (Step 9.8).
std::vector<ClaimType> all_public_claim_types()
{
	return { ClaimType::CapabilityExists, ClaimType::ContractHolds,
		 ClaimType::ArchitectureFollows,
		 ClaimType::FunctionImplements };
}

// Lowercase wire-name matching the MCP schema strings. Mirrors
// parseClaimType in engine_verify_ffi.cpp so the introspection JSON uses
// the same identifiers callers send.
const char *claimTypeWireName(ClaimType t)
{
	switch (t) {
	case ClaimType::CapabilityExists:
		return "capability_exists";
	case ClaimType::ContractHolds:
		return "contract_holds";
	case ClaimType::ArchitectureFollows:
		return "architecture_follows";
	case ClaimType::FunctionImplements:
		return "function_implements";
	}
	return "unknown";
}

// Evidence backend readiness probe (Step 9.5). Verifiers must read
// canonical `entity`/`relation` tables, NOT the deprecated graph_nodes/
// graph_edges. When the canonical tables are empty (e.g. freshly created
// project, pre-index), verifiers must return Unknown + reason instead of
// fabricating a Contradicted/Supported verdict from missing data.
bool evidence_backend_ready(store::GraphStore *store, uint64_t project_id,
			    int64_t *entity_count_out,
			    int64_t *relation_count_out)
{
	if (entity_count_out)
		*entity_count_out = 0;
	if (relation_count_out)
		*relation_count_out = 0;
	if (!store || !store->handle())
		return false;

	sqlite3 *db = store->handle();
	int64_t entities = 0;
	int64_t relations = 0;

	// Entity count: any row for this project.
	{
		const char *sql =
			"SELECT COUNT(*) FROM entity WHERE project_id=?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				entities = sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		} else {
			fprintf(stderr,
				"evidence_backend_ready: entity prepare "
				"failed: %s "
				"[module=verify, method=evidence_backend_ready]\n",
				sqlite3_errmsg(db));
		}
	}
	// Relation count: any typed relation for this project (type=1 is
	// Calls; we count all types because non-Calls evidence is also
	// legitimate for some verifiers, e.g. Defines/Contains).
	{
		const char *sql =
			"SELECT COUNT(*) FROM relation WHERE project_id=?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1,
					   static_cast<int64_t>(project_id));
			if (sqlite3_step(stmt) == SQLITE_ROW)
				relations = sqlite3_column_int64(stmt, 0);
			sqlite3_finalize(stmt);
		} else {
			fprintf(stderr,
				"evidence_backend_ready: relation prepare "
				"failed: %s "
				"[module=verify, method=evidence_backend_ready]\n",
				sqlite3_errmsg(db));
		}
	}

	if (entity_count_out)
		*entity_count_out = entities;
	if (relation_count_out)
		*relation_count_out = relations;
	return entities > 0 && relations > 0;
}

} // namespace verify
