#include "factors.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace resolver
{

namespace
{
/// Fold an ASCII byte to lowercase. SQLite's default LIKE folds only
/// ASCII upper-case letters; all other bytes (including non-ASCII) are
/// returned unchanged so they compare byte-for-byte, matching SQLite.
inline unsigned char likeFold(unsigned char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return static_cast<unsigned char>(ch + ('a' - 'A'));
	return ch;
}

/// Replicate SQLite's default LIKE matching for a full-string pattern.
/// '%' matches any sequence (including empty), '_' matches any single
/// character, and ASCII letters compare case-insensitively. The match
/// is anchored to the whole text (LIKE is not a substring search; the
/// surrounding '%' in callers' patterns provides prefix/suffix freedom).
///
/// This is the standard greedy-with-backtrack wildcard matcher. It is
/// used instead of std::string::find so that '_'/'%' inside a module
/// name and ASCII case differences behave EXACTLY like the original
/// `target_path LIKE '%module_name%'` SQL — preserving identical edges.
bool sqliteLikeMatch(const std::string &pattern, const std::string &text)
{
	size_t p = 0; // pattern cursor
	size_t t = 0; // text cursor
	size_t star_p = std::string::npos; // position of last '%' in pattern
	size_t match_t = 0; // text position aligned with that '%'
	while (t < text.size()) {
		if (p < pattern.size() && pattern[p] == '%') {
			star_p = p;
			match_t = t;
			++p; // tentatively let '%' match zero chars
		} else if (p < pattern.size() &&
			   (pattern[p] == '_' ||
			    likeFold(static_cast<unsigned char>(pattern[p])) ==
				    likeFold(static_cast<unsigned char>(
					    text[t])))) {
			++p;
			++t;
		} else if (star_p != std::string::npos) {
			// backtrack: let the previous '%' swallow one more char
			p = star_p + 1;
			match_t = t = match_t + 1;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '%')
		++p;
	return p == pattern.size();
}

/// Return true if any target_path in `targets` matches the LIKE pattern
/// `"%<module_name>%"` (semantically identical to the original SQL
/// `SELECT COUNT(*) ... WHERE target_path LIKE '%module_name%' > 0`).
bool anyImportMatches(const std::vector<std::string> &targets,
		      const std::string &module_name)
{
	std::string pattern = "%" + module_name + "%";
	for (const auto &target : targets) {
		if (sqliteLikeMatch(pattern, target))
			return true;
	}
	return false;
}

/// Extract the module-name token used for import matching. Mirrors the
/// original factors.cpp logic: strip the file name (last '/'), then take
/// the last directory component. For "a/b/c.go" this yields "b"; for a
/// bare "c.go" it yields "c.go"; for "/c.go" it yields "" (which, like
/// the original SQL `LIKE '%%'`, matches any imported target_path).
std::string moduleTokenFromPath(const std::string &file_path)
{
	std::string module_path = file_path;
	size_t slash = module_path.rfind('/');
	if (slash != std::string::npos)
		module_path = module_path.substr(0, slash);
	size_t prev_slash = module_path.rfind('/');
	if (prev_slash != std::string::npos)
		return module_path.substr(prev_slash + 1);
	return module_path;
}
} // namespace

double factorImportMatch(
	const std::unordered_map<std::string, std::vector<std::string>>
		&import_index,
	const std::string &caller_file, const std::string &candidate_file,
	const std::string &candidate_name)
{
	(void)candidate_name; // unused — kept for caller compatibility

	// Same-module calls don't need an import statement — score 1.0.
	// (Identical to the original early return: pure string comparison,
	// no index lookup, so it is also the fast path for same-directory
	// candidates.)
	{
		size_t c_slash = caller_file.rfind('/');
		size_t t_slash = candidate_file.rfind('/');
		if (c_slash != std::string::npos &&
		    t_slash != std::string::npos) {
			std::string caller_dir = caller_file.substr(0, c_slash);
			std::string cand_dir =
				candidate_file.substr(0, t_slash);
			if (caller_dir == cand_dir)
				return 1.0;
		}
	}

	// Forward check: does the caller's file import the candidate's
	// module? Replaces `SELECT COUNT(*) FROM import WHERE file_path=?
	// AND target_path LIKE '%candidate_module%'` with an in-memory
	// lookup over the pre-loaded import_index.
	std::string candidate_module = moduleTokenFromPath(candidate_file);
	double result = 0.0;
	auto fwd_it = import_index.find(caller_file);
	if (fwd_it != import_index.end() &&
	    anyImportMatches(fwd_it->second, candidate_module))
		result = 1.0;

	// Reverse check: does the candidate's file import the caller's
	// module? Only runs when the forward check found nothing, matching
	// the original `result == 0.0` guard.
	if (result == 0.0) {
		std::string caller_module = moduleTokenFromPath(caller_file);
		auto rev_it = import_index.find(candidate_file);
		if (rev_it != import_index.end() &&
		    anyImportMatches(rev_it->second, caller_module))
			result = 1.0;
	}

	return result;
}

double factorNamespaceMatch(const std::string &caller_file,
			    const std::string &candidate_file)
{
	// Extract directory paths and compare.
	size_t c_slash = caller_file.rfind('/');
	size_t t_slash = candidate_file.rfind('/');
	if (c_slash == std::string::npos || t_slash == std::string::npos)
		return 0.0;

	std::string caller_dir = caller_file.substr(0, c_slash);
	std::string cand_dir = candidate_file.substr(0, t_slash);

	if (caller_dir == cand_dir)
		return kScoreExactMatch; // Same package

	// Check if they share a common parent directory
	// (sibling packages within the same module)
	size_t c_parent = caller_dir.rfind('/');
	size_t t_parent = cand_dir.rfind('/');
	if (c_parent == std::string::npos || t_parent == std::string::npos)
		return 0.0;

	std::string caller_parent = caller_dir.substr(0, c_parent);
	std::string cand_parent = cand_dir.substr(0, t_parent);
	if (caller_parent == cand_parent && caller_parent.length() > 0)
		return kScoreSiblingModule; // Sibling packages

	return 0.0;
}

double factorDistanceMatch(const std::string &caller_file,
			   const std::string &candidate_file)
{
	if (caller_file == candidate_file)
		return kScoreExactMatch; // Same file

	size_t c_slash = caller_file.rfind('/');
	size_t t_slash = candidate_file.rfind('/');
	if (c_slash == std::string::npos || t_slash == std::string::npos)
		return 0.0;

	std::string caller_dir = caller_file.substr(0, c_slash);
	std::string cand_dir = candidate_file.substr(0, t_slash);
	if (caller_dir == cand_dir)
		return kScoreSameDirectory; // Same directory

	return 0.0;
}

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

double factorReceiverMatch(const std::string &ref_name,
			   const std::string &caller_file,
				   const std::string &candidate_name,
				   const std::string &candidate_file)
{
	// Receiver match: for method calls like a.method(),
	// check if the candidate is a method of the caller's receiver type.
	// This is a simplified check: if caller and candidate share
	// the same package prefix, boost the score.
	size_t c_slash = caller_file.rfind('/');
	size_t t_slash = candidate_file.rfind('/');
	if (c_slash == std::string::npos || t_slash == std::string::npos)
		return 0.0;

	std::string caller_pkg = caller_file.substr(0, c_slash);
	std::string cand_pkg = candidate_file.substr(0, t_slash);

	// Same package: likely receiver match
	if (caller_pkg == cand_pkg)
		return kScoreExactMatch;

	// Different package: check if candidate is in a sub-package
	if (cand_pkg.find(caller_pkg) == 0)
		return kScorePartialMatch;

	return 0.0;
}

// Step 5 (plan §5.3): receiver type evidence factor.
//
// Replaces the directory-heuristic factorReceiverMatch with actual
// type-based matching. The key insight: when a reference carries a
// known receiver_type (e.g. "Box" from `let b: Box = ...; b.draw()`),
// the candidate's qualified_name should contain that type as a prefix
// (e.g. "Box::draw", "Box.draw", "Box::draw"). This is strong
// structural evidence — far more reliable than "same directory".
//
// When receiver_type is empty (dynamic/unknown receiver), we return
// 0.5 (neutral) rather than 0.0. This is critical: returning 0.0 would
// penalize ALL candidates equally (no differentiation), while 0.5
// ensures the receiver factor does not distort the ranking when we
// lack type evidence. The decision then falls to other factors
// (Import, Namespace, Signature) as before.
double factorReceiverTypeMatch(const std::string &receiver_type,
			       const std::string &candidate_qname,
			       const std::string &candidate_name,
			       const std::string &candidate_file)
{
	// No receiver type evidence → neutral, do not fabricate evidence.
	if (receiver_type.empty())
		return 0.5;

	// Strong match: qualified_name contains the receiver type as a
	// prefix. Covers "Box::draw", "Box.draw", "MyClass::method", etc.
	if (!candidate_qname.empty()) {
		// Check "Type::method" and "Type.method" patterns.
		std::string prefix1 = receiver_type + "::";
		std::string prefix2 = receiver_type + ".";
		if (candidate_qname.find(prefix1) == 0 ||
		    candidate_qname.find(prefix2) == 0)
			return kScoreExactMatch;
		// Also check if receiver_type appears as a component anywhere
		// in the qualified_name (e.g. "module::Box::draw").
		if (candidate_qname.find(prefix1) != std::string::npos ||
		    candidate_qname.find(prefix2) != std::string::npos)
			return kScorePartialMatch;
	}

	// Weak fallback: if the candidate's file path contains the receiver
	// type name (e.g. file "box.go" containing methods of Box), give a
	// partial score. This is less reliable than qualified_name but
	// better than nothing for languages that don't populate
	// qualified_name (e.g. Go, where methods are defined as
	// `func (b Box) draw()` and qualified_name may be empty).
	size_t slash = candidate_file.rfind('/');
	std::string fname = (slash != std::string::npos) ?
				    candidate_file.substr(slash + 1) :
				    candidate_file;
	// Convert to lowercase for case-insensitive comparison (Go file
	// names are typically lowercase: "box.go", "renderer.ts").
	std::string fname_lower = fname;
	std::string rtype_lower = receiver_type;
	for (auto &ch : fname_lower)
		ch = static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	for (auto &ch : rtype_lower)
		ch = static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	if (!rtype_lower.empty() &&
	    fname_lower.find(rtype_lower) != std::string::npos)
		return kScorePartialMatch;

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