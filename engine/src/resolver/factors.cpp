#include "factors.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace resolver
{

double factorSignatureMatch(int caller_arity, int candidate_arity)
{
	if (caller_arity == 0 && candidate_arity == 0)
		return kScorePartialMatch; // Both unknown arity
	if (caller_arity == candidate_arity)
		return kScoreExactMatch; // Exact match
	if (candidate_arity == 0)
		return kScorePartialMatch; // Candidate has unknown arity
	return kScorePenalty; // Known-different arity — penalty
}

double factorConstructorMatch(const std::string &ref_name,
			      const std::string &candidate_name,
			      int candidate_kind)
{
	// Constructor match: reference name matches a class/struct name.
	// candidate_kind: 2 = Class, 3 = Struct
	if (candidate_kind != 2 && candidate_kind != 3)
		return 0.0;
	if (ref_name == candidate_name)
		return kScoreExactMatch; // Exact class/struct name match
	return 0.0;
}

// v0.2.5 (perf): pre-parsed receiver matching. Build the ref-level context
// (prefix1/prefix2/rtype_lower) once per reference so the resolver hot loop
// does not reallocate them for every candidate.
//
// This builder sits between two scorers that were deleted as dead code
// (factorReceiverMatch, factorReceiverTypeMatch) — it is NOT dead itself:
// pipeline_apply.cpp calls it once per reference. Kept here, adjacent to the
// scorer that consumes the context.
ReceiverMatchContext buildReceiverMatchContext(const std::string &receiver_type)
{
	ReceiverMatchContext ctx;
	if (receiver_type.empty()) {
		ctx.empty = true;
		return ctx;
	}
	ctx.prefix1 = receiver_type + "::";
	ctx.prefix2 = receiver_type + ".";
	ctx.rtype_lower = receiver_type;
	for (auto &ch : ctx.rtype_lower)
		ch = static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	return ctx;
}

double factorReceiverTypeMatchPrecomp(const ReceiverMatchContext &ctx,
				      const std::string &candidate_qname,
				      const std::string &candidate_file)
{
	if (ctx.empty)
		return 0.5; // neutral, no receiver evidence

	// Strong match: qualified_name contains the receiver type as a prefix.
	if (!candidate_qname.empty()) {
		if (candidate_qname.find(ctx.prefix1) == 0 ||
		    candidate_qname.find(ctx.prefix2) == 0)
			return kScoreExactMatch;
		if (candidate_qname.find(ctx.prefix1) != std::string::npos ||
		    candidate_qname.find(ctx.prefix2) != std::string::npos)
			return kScorePartialMatch;
	}

	// Weak fallback: candidate file basename contains the lowercased
	// receiver type (mirrors factorReceiverTypeMatch lines 292-308).
	size_t slash = candidate_file.rfind('/');
	std::string fname = (slash != std::string::npos) ?
				    candidate_file.substr(slash + 1) :
				    candidate_file;
	std::string fname_lower = fname;
	for (auto &ch : fname_lower)
		ch = static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	// A filename coincidence is weaker evidence than a qualified_name that
	// really contains the receiver type, so it gets its own value instead of
	// kScorePartialMatch: returning 0.5 here made `Widgets.cpp` — a file whose
	// name happens to mention the type — score exactly the same as a candidate
	// whose qualified_name contains it, which is the difference between a
	// coincidence and receiver evidence.
	if (!ctx.rtype_lower.empty() &&
	    fname_lower.find(ctx.rtype_lower) != std::string::npos)
		return kScoreWeakFilenameMatch;

	return 0.0;
}

double factorCommonNamePenalty(const std::string &name)
{
	static const std::unordered_set<std::string> kCommonNames = {
		"Len",	     "Init",	"Run",	    "Stop",    "Close",
		"Open",	     "Read",	"Write",    "Get",     "Set",
		"Add",	     "Remove",	"Update",   "Delete",  "Create",
		"New",	     "Process", "Handle",   "Execute", "Start",
		"End",	     "Error",	"String",   "Format",  "Marshal",
		"Unmarshal", "Equals",	"Compare",  "Hash",    "Copy",
		"Clone",     "Reset",	"Clear",    "IsEmpty", "IsValid",
		"HasNext",   "Next",	"Previous", "First",   "Last",
		"Value",     "Key",	"Int",	    "Float",   "Bool",
		"Bytes",     "Size",	"Cap",
	};
	if (kCommonNames.count(name) > 0)
		return kCommonNamePenaltyValue;
	return 0.0;
}

// ─── Language-specific visibility check ─────────────────────────────
//
// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
//   internal/cbm/helpers.c :: cbm_is_exported()
//
// Many languages have visibility rules that prevent cross-module calls
// to unexported/private symbols. The Resolver Pipeline cannot resolve
// these, so they must be rejected at the scoring stage.
//
// Go:  names starting with lowercase are unexported (package-private)
// Python: names starting with '_' are private
// Java/Kotlin/C#: names starting with lowercase are package-private
//       (not strictly true for all cases, but a safe heuristic)
// C/C++: no visibility-based restriction at the language level
double factorVisibilityCheck(const std::string &language,
			     const std::string &candidate_name,
			     const std::string &caller_file,
			     const std::string &candidate_file)
{
	if (candidate_name.empty())
		return 1.0; // Unknown — allow

	char first = candidate_name[0];

	// Go: unexported names (lowercase) cannot be called from ANOTHER
	// package — this is a hard language rule. BUT they ARE callable within
	// the SAME package (Go's unexported == package-private). A Go package
	// maps 1:1 to a directory, so caller and candidate in the same directory
	// are the same package and the unexported symbol is visible. Only
	// cross-package (different directory) unexported calls are rejected.
	if (language == "go" && first >= 'a' && first <= 'z') {
		size_t c_slash = caller_file.rfind('/');
		size_t t_slash = candidate_file.rfind('/');
		std::string c_dir = (c_slash == std::string::npos) ?
					    "" :
					    caller_file.substr(0, c_slash);
		std::string t_dir = (t_slash == std::string::npos) ?
					    "" :
					    candidate_file.substr(0, t_slash);
		if (c_dir == t_dir)
			return 1.0; // same package — unexported is accessible
		return 0.0; // cross-package — reject unexported Go symbol
	}

	// Python: names starting with '_' are private (module-level
	// convention). They ARE accessible within the same module —
	// only reject for cross-module resolution. Previously this
	// rejected ALL '_'-prefixed names unconditionally, which
	// dropped legitimate intra-class/intra-module calls like
	// render() → self._load_data() (Bug 2 in res.md).
	if (language == "python" && first == '_') {
		if (caller_file == candidate_file)
			return 1.0; // Same module — private is accessible
		return 0.0; // Cross-module — reject
	}

	// Java/Kotlin/C#: names starting with lowercase are typically
	// package-private or instance methods — heuristic rejection for
	// cross-module bare-name matches (qualified calls like obj.method()
	// are handled by the parser, not the Resolver Pipeline).
	if ((language == "java" || language == "kotlin" ||
	     language == "csharp") &&
	    first >= 'a' && first <= 'z')
		return 0.5; // Weak penalty, not hard rejection

	return 1.0; // Visible — allow
}

// ─── C/C++ definition-priority helpers ─────────────────────────────
//
// Reference: Code Review Finding #8 — README promises that «.c/.cpp
// definitions are preferred over .h prototypes», but neither the
// multi-factor scorer nor project_resolver::rankCandidate distinguished
// definition vs declaration. Previously, when a function was both
// declared in a header and defined in a source file, both candidates
// scored identically and the tie was broken by the arbitrary
// entity_index insertion order, so C/C++ call targets were frequently
// wrong.
//
// The disambiguation is purely extension-based, matching the documented
// promise: a .c/.cpp/... translation unit is a definition site; a
// .h/.hpp/... header is a (typically) declaration/prototype site. This
// is intentionally simple and language-agnostic at the file level.

/// Lowercase the substring after the last '.' (the file extension).
static std::string lowerExtension(const std::string &file_path)
{
	size_t dot = file_path.rfind('.');
	if (dot == std::string::npos)
		return "";
	std::string lower;
	lower.reserve(file_path.size() - dot);
	for (size_t i = dot; i < file_path.size(); ++i)
		lower.push_back(static_cast<char>(std::tolower(
			static_cast<unsigned char>(file_path[i]))));
	return lower;
}

bool isCppSourceFile(const std::string &file_path)
{
	std::string ext = lowerExtension(file_path);
	return ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
	       ext == ".c++" || ext == ".m" || ext == ".mm";
}

bool isCppHeaderFile(const std::string &file_path)
{
	std::string ext = lowerExtension(file_path);
	return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" ||
	       ext == ".h++" || ext == ".inl" || ext == ".ipp" ||
	       ext == ".tpp" || ext == ".tcc";
}

double factorDefinitionMatch(const std::string &language,
			     const std::string &candidate_file)
{
	// Only meaningful for C/C++. Other languages have no header/source
	// split that implies definition-priority, so stay neutral (1.0) to
	// avoid perturbing their ranking or resolution threshold.
	if (language != "cpp")
		return 1.0;

	if (isCppSourceFile(candidate_file))
		return kScoreExactMatch; // 1.0 — boost source-file definition
	if (isCppHeaderFile(candidate_file))
		return kScorePenalty; // -0.5 — lower header-only prototype
	return 1.0; // unrecognized cpp extension — neutral
}

} // namespace resolver