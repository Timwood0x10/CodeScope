#include "resolve_cache.h"

namespace resolver
{

CachingResolver::CachingResolver(std::unique_ptr<Resolver> inner)
	: inner_(std::move(inner))
{
}

void CachingResolver::setFile(const std::string &file_path)
{
	// Per-file caches are keyed by file_path already — no explicit clear needed.
	// resolve_cache_ entries are naturally scoped by (file, name) key.
	(void)file_path;
}

void CachingResolver::addImport(const std::string &file_path,
				const std::string &imported_module)
{
	reach_cache_[file_path].insert(imported_module);
}

void CachingResolver::addImportMap(const std::string &prefix,
				   const std::string &module_scope)
{
	import_map_cache_[prefix] = module_scope;
}

ResolutionResult CachingResolver::resolve(const std::string &name,
					  const std::string &file_path,
					  const ir::Node *context)
{
	// Check resolve_cache first: (file, name) → last resolved result
	CacheKey key{ file_path, name };
	{
		auto it = resolve_cache_.find(key);
		if (it != resolve_cache_.end()) {
			stats_.resolve_cache_hit++;
			return it->second;
		}
		stats_.resolve_cache_miss++;
	}

	// Check reach_cache: is name reachable from this file's imports?
	{
		auto file_it = reach_cache_.find(file_path);
		if (file_it != reach_cache_.end()) {
			// Quick prefix match: name like "fmt.Println" → check if "fmt" is imported
			auto dot = name.find('.');
			if (dot != std::string::npos) {
				std::string prefix = name.substr(0, dot);
				if (file_it->second.find(prefix) !=
				    file_it->second.end()) {
					stats_.reach_cache_hit++;
					// Known import — proceed to inner resolve
				}
			}
		}
	}

	// Check import_map_cache: map import prefix to module scope
	{
		auto dot = name.find('.');
		if (dot != std::string::npos) {
			std::string prefix = name.substr(0, dot);
			auto it = import_map_cache_.find(prefix);
			if (it != import_map_cache_.end()) {
				stats_.import_map_hit++;
				// prefix known, proceed to inner resolve with context
			}
		}
	}

	// Fall through to inner resolver
	ResolutionResult result = inner_->resolve(name, file_path, context);

	// Cache the result for this (file, name) pair
	resolve_cache_[key] = result;
	stats_.resolve_cache_stored++;

	return result;
}

} // namespace resolver
