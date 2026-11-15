#ifndef CODESCOPE_UTIL_SYMBOL_IDENTITY_H
#define CODESCOPE_UTIL_SYMBOL_IDENTITY_H

namespace util
{

/// Measured C-family callable identity, shared by every reader of the entity
/// table. Aliases must be `entity` (the seed row) and `e2` (the peer row).
///
/// Only a non-empty measured signature may claim that two rows are the same
/// callable. Kind 0/1 is one family because the visitor emits out-of-class
/// methods as Functions; language is family-wide because a .h header is
/// detected as 'cpp' while its .c definition is detected as 'c'.
///
/// It lives in util rather than in the query layer so that the views and the
/// query engine can share one spelling without depending on each other.
inline constexpr const char *kSameCallable =
	"e2.project_id=entity.project_id "
	"AND entity.kind IN (0,1) AND e2.kind IN (0,1) "
	"AND entity.language IN ('c','cpp','c++') "
	"AND e2.language IN ('c','cpp','c++') "
	"AND e2.name=entity.name "
	"AND COALESCE(NULLIF(e2.qualified_name,''),e2.name)="
	"COALESCE(NULLIF(entity.qualified_name,''),entity.name) "
	"AND entity.callable_signature!='' "
	"AND e2.callable_signature=entity.callable_signature ";

} // namespace util

#endif // CODESCOPE_UTIL_SYMBOL_IDENTITY_H
