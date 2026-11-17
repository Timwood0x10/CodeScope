#ifndef CODESCOPE_RESOLVER_TYPE_SEGMENT_H
#define CODESCOPE_RESOLVER_TYPE_SEGMENT_H

#include <string>

namespace resolver
{

/// True when `type` occurs in `qname` as a complete type segment followed by
/// a member separator.
///
/// Both "Type.member" and "pkg.Type.member" match type "Type";
/// "OtherRegistry.member" does not. A prefix-only test missed the
/// package-qualified form (the type is no longer at position 0), and a plain
/// substring test accepted a longer type whose name ends with it.
///
/// @param qname Qualified name ("pkg.Type.member" or "Type::member").
/// @param type Type name to look for ("Registry").
/// @return True when the type starts a segment and is followed by '.' or
///         '::'.
inline bool MatchesTypeSegment(const std::string &qname,
			       const std::string &type)
{
	if (type.empty() || qname.size() <= type.size())
		return false;
	size_t at = qname.find(type);
	while (at != std::string::npos) {
		const size_t after = at + type.size();
		if (after >= qname.size())
			return false;
		const bool starts_segment = at == 0 || qname[at - 1] == '.' ||
					    qname[at - 1] == ':';
		const char sep = qname[after];
		const bool followed = sep == '.' ||
				      (sep == ':' && after + 1 < qname.size() &&
				       qname[after + 1] == ':');
		if (starts_segment && followed)
			return true;
		at = qname.find(type, at + 1);
	}
	return false;
}

} // namespace resolver

#endif // CODESCOPE_RESOLVER_TYPE_SEGMENT_H
