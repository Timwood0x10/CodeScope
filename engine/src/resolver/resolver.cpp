#include "resolver.h"

namespace resolver
{

void ResolverChain::addResolver(std::unique_ptr<Resolver> resolver)
{
	if (resolver)
		resolvers_.push_back(std::move(resolver));
}

ResolutionResult ResolverChain::resolve(const std::string &name,
					const std::string &file_path,
					const ir::Node *context)
{
	for (auto &r : resolvers_) {
		if (!r)
			continue;
		ResolutionResult result = r->resolve(name, file_path, context);
		if (result.status != ResolutionStatus::NotFound) {
			return result;
		}
	}

	ResolutionResult not_found;
	not_found.name = name;
	not_found.status = ResolutionStatus::NotFound;
	not_found.resolver_name = "resolver_chain";
	return not_found;
}

} // namespace resolver
