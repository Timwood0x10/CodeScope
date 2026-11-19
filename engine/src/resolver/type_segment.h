#ifndef CODESCOPE_RESOLVER_TYPE_SEGMENT_H
#define CODESCOPE_RESOLVER_TYPE_SEGMENT_H

#include <string>

namespace resolver
{

/// True when `type` is the member's owning type in `qname`.
///
/// Only the segment immediately before the member counts:
/// "pkg.Type.member" matches "Type" but not "pkg", and "Outer::Inner::m"
/// matches "Inner" but not "Outer". Reading any segment as the type let a
/// package name stand in for the receiver — "worker.Decoy.Run" matched
/// "worker" — which attributed a dispatch edge to the wrong owner.
///
/// @param qname Qualified name ("pkg.Type.member" or "Type::member").
/// @param type Type name to look for ("Registry").
/// @return True when `type` ends right before the member separator.
inline bool MatchesTypeSegment(const std::string &qname,
			       const std::string &type)
{
	if (type.empty())
		return false;
	// The member separator is the last '.' or '::'; '::' wins when it is
	// closer to the end.
	size_t member = qname.rfind('.');
	size_t sep_len = 1;
	const size_t colons = qname.rfind("::");
	if (colons != std::string::npos &&
	    (member == std::string::npos || colons > member)) {
		member = colons;
		sep_len = 2;
	}
	if (member == std::string::npos || member == 0 ||
	    member + sep_len >= qname.size() || member < type.size())
		return false;
	const size_t begin = member - type.size();
	if (qname.compare(begin, type.size(), type) != 0)
		return false;
	return begin == 0 || qname[begin - 1] == '.' || qname[begin - 1] == ':';
}

} // namespace resolver

#endif // CODESCOPE_RESOLVER_TYPE_SEGMENT_H
