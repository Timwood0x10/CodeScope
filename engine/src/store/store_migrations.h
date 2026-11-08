#ifndef CODESCOPE_STORE_MIGRATIONS_H
#define CODESCOPE_STORE_MIGRATIONS_H

#include <cstddef>
#include <cstdint>

namespace store
{

/// One ordered schema migration.
///
/// `version` is the resulting PRAGMA user_version, `sql` may contain several
/// statements and is executed inside a single transaction by
/// GraphStore::applyPendingMigrations(). Versions are dense and append-only:
/// an applied migration is never edited, only superseded by a new one.
///
/// The table lives in store_migrations_data.cpp so this header stays small and
/// the 1000-line budget of code_rules §1.1 is respected as migrations grow.
struct SchemaMigration {
	int64_t version;
	const char *name;
	const char *sql;
};

/// Ordered migration table. `count` receives the number of entries.
/// Versions start at 2: version 1 is the pre-0.3.0 baseline that
/// createSchema() already produces.
const SchemaMigration *schemaMigrations(std::size_t &count);

/// Highest version in the table (0 when empty).
int64_t latestSchemaVersion();

} // namespace store

#endif // CODESCOPE_STORE_MIGRATIONS_H
