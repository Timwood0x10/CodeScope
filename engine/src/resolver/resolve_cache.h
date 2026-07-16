#ifndef RESOLVE_CACHE_H
#define RESOLVE_CACHE_H

#include "resolver.h"
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace resolver
{

/**
 * Resolve cache trio: reachability, import map, and per-file-name cache.
 *
 * Wraps an existing Resolver and adds three caches:
 * 1. reach_cache:      per-file imported module set (which files are reachable)
 * 2. import_map_cache: import prefix → module/symbol scope
 * 3. resolve_cache:    (caller_file, callee_name) → last result, avoids repeat work
 *
 * All caches are file-scoped and cleared when the file changes.
 */
class CachingResolver : public Resolver {
    public:
	explicit CachingResolver(std::unique_ptr<Resolver> inner);

	ResolutionResult resolve(const std::string &name,
				 const std::string &file_path,
				 const ir::Node *context) override;

	const char *name() const override
	{
		return "caching_resolver";
	}

	/// Switch to a new file — clears per-file caches.
	void setFile(const std::string &file_path);

	/// Record that `file_path` imports from `imported_module`.
	void addImport(const std::string &file_path,
		       const std::string &imported_module);

	/// Record that `prefix` maps to `module_scope`.
	void addImportMap(const std::string &prefix,
			  const std::string &module_scope);

	/// Cache stats for observability.
	struct Stats {
		uint64_t reach_cache_hit = 0;
		uint64_t import_map_hit = 0;
		uint64_t resolve_cache_hit = 0;
		uint64_t resolve_cache_miss = 0;
		uint64_t resolve_cache_stored = 0;
	};
	const Stats &stats() const
	{
		return stats_;
	}
	void resetStats()
	{
		stats_ = Stats{};
	}

    private:
	std::unique_ptr<Resolver> inner_;

	// Key:   caller_file_path
	// Value: set of imported module/file paths
	std::unordered_map<std::string, std::unordered_set<std::string>>
		reach_cache_;

	// Key:   import prefix (e.g. "os", "fmt")
	// Value: resolved module scope
	std::unordered_map<std::string, std::string> import_map_cache_;

	// Key:   (caller_file, callee_name)
	// Value: cached resolution result
	struct CacheKey {
		std::string file;
		std::string name;
		bool operator==(const CacheKey &o) const
		{
			return file == o.file && name == o.name;
		}
	};
	struct CacheKeyHash {
		size_t operator()(const CacheKey &k) const
		{
			return std::hash<std::string>{}(k.file) ^
			       (std::hash<std::string>{}(k.name) << 1);
		}
	};
	std::unordered_map<CacheKey, ResolutionResult, CacheKeyHash>
		resolve_cache_;

	Stats stats_;
};

} // namespace resolver

#endif // RESOLVE_CACHE_H
