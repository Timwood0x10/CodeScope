#ifndef CODESCOPE_RESOLVER_FACTORS_H
#define CODESCOPE_RESOLVER_FACTORS_H

#include <string>
#include <vector>
#include <cstdint>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace resolver
{

/// Infer the source language from a file path's extension. Returns "" when
/// the extension is unrecognized.
///
/// NOTE: every C/C++ extension — including `.c` — maps to "cpp". The
/// visitors, by contrast, label a `.c` translation unit "c", so callers
/// that compare this value against an entity's language MUST use
/// languagesCompatible() rather than string equality.
inline std::string languageFromPath(const std::string &file_path)
{
	size_t dot = file_path.rfind('.');
	if (dot == std::string::npos)
		return "";
	std::string ext = file_path.substr(dot);
	// Normalize to lowercase for case-insensitive comparison.
	std::string lower;
	lower.reserve(ext.size());
	for (char ch : ext)
		lower.push_back(static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch))));
	if (lower == ".cpp" || lower == ".cc" || lower == ".cxx" ||
	    lower == ".c" || lower == ".h" || lower == ".hpp" ||
	    lower == ".hh" || lower == ".hxx")
		return "cpp";
	if (lower == ".rs")
		return "rust";
	if (lower == ".py")
		return "python";
	if (lower == ".go")
		return "go";
	if (lower == ".ts" || lower == ".tsx")
		return "typescript";
	if (lower == ".js" || lower == ".jsx")
		return "javascript";
	if (lower == ".java")
		return "java";
	return "";
}

/// True when two language labels may describe the same code.
///
/// The labels come from two different vocabularies: languageFromPath()
/// reports every C/C++ extension (including `.c`) as "cpp", while the
/// visitors label a `.c` translation unit "c" and a `.h` header may belong
/// to either. Comparing the raw strings would therefore reject every
/// legitimate C call site, so the whole C family is treated as one
/// language.
///
/// The JS/TS family needs the same treatment for the same reason, and this
/// was a measured false-negative class rather than a hypothetical one:
/// languageFromPath() reports a `.tsx` path as "typescript" while the tsx
/// visitor labels the entities it defines "tsx", so the two labels never
/// matched. On two real TypeScript projects the resolver produced ZERO call
/// edges — AIScope (220 tsx + 3 typescript entities, 1164 references of
/// which 37 name locally-defined symbols) and PolyScope (27 tsx + 14
/// typescript, 80 references, 9 naming local symbols). A `.ts` module
/// calling a `.tsx` component, or a `.js` file calling into either, is the
/// ordinary shape of a React/TS codebase; the language label is one gate
/// among several (name, arity, directory, import evidence all still apply),
/// so treating the family as one language cannot manufacture edges on its
/// own.
///
/// Everything else must match exactly; an empty label means "unknown" and is
/// always allowed through (never over-filter).
inline bool languagesCompatible(const std::string &a, const std::string &b)
{
	if (a.empty() || b.empty() || a == b)
		return true;
	static const std::unordered_set<std::string> kCFamily = {
		"c", "cpp", "c++", "cxx", "objc", "objective-c",
	};
	static const std::unordered_set<std::string> kJsFamily = {
		"javascript",
		"typescript",
		"tsx",
	};
	if (kCFamily.count(a) > 0 && kCFamily.count(b) > 0)
		return true;
	return kJsFamily.count(a) > 0 && kJsFamily.count(b) > 0;
}

/// True when `candidate_file` is the module named by a RELATIVE import
/// specifier written in `caller_dir`.
///
/// This is the evidence TypeScript/JavaScript projects never had. Their calls
/// are bare names (`helper()`), the language has no receiver to match on, and
/// the only import signal the visitors produced was a *plain set of imported
/// names* — which says "imported" but not "imported from here". So every
/// cross-directory call had no factor that could separate the candidates and
/// the resolver abstained (measured: PolyScope 0 edges from 80 references).
/// The `import` table does store the module specifier
/// (`./helper` → alias `helper`), so the missing piece was this comparison.
///
/// `./helper` names helper.ts / helper.tsx / helper.js, so the match is
/// extension-agnostic, and it accepts a directory match (`./utils` →
/// `utils/index.ts`). A bare package specifier ("react", "@/lib/utils") names
/// no file inside the project and returns false: the caller must not invent
/// path evidence from an alias it cannot resolve.
inline bool relativeImportMatchesFile(const std::string &caller_dir,
				      const std::string &spec,
				      const std::string &candidate_file)
{
	if (spec.empty() || spec[0] != '.')
		return false;
	// Join the specifier onto the caller's directory and collapse the "." and
	// ".." segments, so "../lib/x" and "./y" compare against real paths.
	const std::string joined = caller_dir.empty() ? spec :
							caller_dir + "/" + spec;
	std::vector<std::string> parts;
	size_t i = 0;
	while (i <= joined.size()) {
		const size_t slash = joined.find('/', i);
		const std::string seg = joined.substr(
			i, slash == std::string::npos ? std::string::npos :
							slash - i);
		if (seg == "..") {
			if (!parts.empty())
				parts.pop_back();
		} else if (!seg.empty() && seg != ".") {
			parts.push_back(seg);
		}
		if (slash == std::string::npos)
			break;
		i = slash + 1;
	}
	std::string base;
	for (const auto &p : parts) {
		if (!base.empty())
			base += '/';
		base += p;
	}
	if (base.empty())
		return false;
	// The collapse above drops the empty first segment of an absolute path;
	// put the root back or no absolute candidate can ever match.
	if (joined[0] == '/')
		base.insert(base.begin(), '/');
	auto strip_ext = [](const std::string &p) -> std::string {
		const size_t dot = p.rfind('.');
		const size_t slash = p.rfind('/');
		if (dot != std::string::npos &&
		    (slash == std::string::npos || dot > slash))
			return p.substr(0, dot);
		return p;
	};
	if (candidate_file == base || strip_ext(candidate_file) == base)
		return true;
	// `./utils` may name a directory: any file inside it can be that module's
	// entry point (index.ts and friends).
	return candidate_file.rfind(base + "/", 0) == 0;
}

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
/// SQL LIKE predicates — preserving identical edges.
inline bool sqliteLikeMatch(const std::string &pattern, const std::string &text)
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

// ── Named constants for factor weights ──────────────────────────────
constexpr double kWeightModuleMatch = 0.15;
constexpr double kWeightImportMatch = 0.80; // Dominant for cross-module
// The caller imports this callee from a RELATIVE module and the candidate is
// that module: exact path evidence, so it outweighs the general import signal.
// Only applied when such a specifier was actually recorded — see
// relativeImportMatchesFile.
constexpr double kWeightImportModuleMatch = 0.90;
constexpr double kWeightNamespaceMatch = 0.10;
constexpr double kWeightSignatureMatch = 0.10;
constexpr double kWeightDistanceMatch = 0.05;
constexpr double kWeightConstructorMatch = 0.10;
constexpr double kWeightReceiverMatch = 0.15;
constexpr double kWeightCommonNamePenalty = 0.10;
constexpr double kWeightCallKindMatch = 0.15; // call_kind-based boost/penalty
constexpr double kWeightDefinitionMatch =
	0.20; // C/C++ source def over header proto

// ── Named constants for call_kind values (matches ir::CallKind enum) ──
constexpr int kCallKindDirect = 0;
constexpr int kCallKindMethod = 1;
constexpr int kCallKindInterface = 2;
constexpr int kCallKindConstructor = 3;
constexpr int kCallKindStaticMethod = 4;
constexpr int kCallKindVirtual = 5;

// ── Named constants for scoring values ──────────────────────────────
constexpr double kScoreExactMatch = 1.0;
constexpr double kScorePartialMatch = 0.5;
constexpr double kScorePenalty = -0.5;
constexpr double kScoreSiblingModule = 0.5;
constexpr double kScoreSameDirectory = 0.3;
// A candidate whose FILE NAME happens to contain the receiver type. Weaker
// than a qualified_name that really contains it (kScorePartialMatch), and it
// must not be confused with that hit: the two used to be the same value, so a
// filename coincidence was indistinguishable from receiver evidence.
constexpr double kScoreWeakFilenameMatch = 0.2;

// ── Threshold ───────────────────────────────────────────────────────
constexpr double kResolutionThreshold = 0.40;

// Step 5 (plan §5.4): ambiguity gate margin. The top-1 candidate must
// lead the top-2 candidate by at least this much to produce a single-
// target CALLS edge. If the margin is not met, the reference is marked
// ambiguous and no CALLS edge is written (conservative abstain).
constexpr double kAmbiguityMargin = 0.15;

// Step 5 (plan §5.6): absolute threshold for evidence-gated fuzzy
// fallback. Fuzzy matches must clear this higher bar (vs 0.40 for
// exact-name) because fuzzy name similarity is inherently weaker.
constexpr double kFuzzyResolutionThreshold = 0.55;

// ── Common name penalty value ───────────────────────────────────────
constexpr double kCommonNamePenaltyValue = 0.25;

/// A single factor's scoring result.
struct FactorResult {
	std::string name; // Factor name, e.g. "ImportMatch", "NamespaceMatch"
	double weight; // Factor weight (0.0 - 1.0)
	double score; // Match score (0.0 - 1.0)
	std::string detail; // Human-readable explanation
};

/// Map a scoring factor name — the ones applyConstraints accumulates, see
/// pipeline_apply.cpp — to the `resolution_kind` recorded on the edge.
///
/// The kind answers "which evidence decided this match", and the label is what
/// per-kind accuracy audits group by, so it must name the decisive evidence
/// rather than whichever reference field happened to be non-empty. An empty
/// result means the factor is unknown to this mapping, and the caller falls
/// back to its own labelling.
inline std::string resolutionKindFromFactor(const std::string &factor)
{
	if (factor == "ReceiverMatch")
		return "receiver_type";
	if (factor == "ImportMatch")
		return "imported";
	// A distinct label from the general import signal: this one means the
	// candidate IS the module the caller imported the name from, which is a
	// stronger and auditable statement than "the caller has imports".
	if (factor == "ImportModuleMatch")
		return "import_module";
	if (factor == "SignatureMatch")
		return "signature";
	if (factor == "DefinitionMatch")
		return "definition";
	if (factor == "ConstructorMatch")
		return "constructor";
	if (factor == "CallKindMatch")
		return "call_kind";
	if (factor == "DistanceMatch")
		return "distance";
	if (factor == "NamespaceMatch")
		return "namespace";
	if (factor == "ModuleMatch")
		return "module";
	return "";
}

/// Check if the caller's file imports the candidate's module (forward)
/// or the candidate's file imports the caller's module (reverse).
/// Returns 1.0 if an import relationship is found, 0.0 otherwise.
///
/// Uses the pre-loaded import_index (file_path -> list of target_path
/// strings) instead of per-candidate SQL queries, eliminating the
/// non-sargable `target_path LIKE '%module_name%'` full table scans
/// that dominated ResolverPipeline::run() (~174s for 108k refs).
///
/// Matching replicates the original SQL
/// `target_path LIKE '%<module_name>%'` EXACTLY: '%' matches any
/// sequence (including empty), '_' matches any single character, and
/// ASCII letters compare case-insensitively (SQLite's default LIKE
/// behavior; non-ASCII bytes compare as-is). This preserves IDENTICAL
/// resolved edges versus the previous SQL-based implementation.
///
/// @param import_index   Pre-loaded map: file_path -> all import
///                       target_path strings recorded for that file.
/// @param caller_file    File path where the call site resides.
/// @param candidate_file File path of the candidate entity.
/// @param candidate_name Name of the candidate (retained for callers;
///                       not used by the matching logic).
double factorImportMatch(
	const std::unordered_map<std::string, std::vector<std::string>>
		&import_index,
	const std::string &caller_file, const std::string &candidate_file,
	const std::string &candidate_name);

/// Check if caller and candidate share the same namespace/module.
/// Returns 1.0 if same module, 0.5 if sibling module, 0.0 otherwise.
double factorNamespaceMatch(const std::string &caller_file,
			    const std::string &candidate_file);

/// Check if the candidate is in the same file as the caller.
/// Returns 1.0 if same file, 0.3 if same directory, 0.0 otherwise.
double factorDistanceMatch(const std::string &caller_file,
			   const std::string &candidate_file);

/// Check if arity (parameter count) matches.
/// Returns 1.0 if exact match, 0.5 if candidate has unknown arity (0),
/// -0.5 if known-different arity, 0.0 otherwise.
double factorSignatureMatch(int caller_arity, int candidate_arity);

/// Check if the candidate is a constructor for a class/struct.
/// Returns 1.0 if candidate kind is Class/Struct and name matches.
double factorConstructorMatch(const std::string &ref_name,
			      const std::string &candidate_name,
			      int candidate_kind);

/// Check if the candidate is a receiver method for the caller's type.
/// Returns 1.0 if receiver type matches, 0.0 otherwise.
double factorReceiverMatch(const std::string &ref_name,
			   const std::string &caller_file,
			   const std::string &candidate_name,
			   const std::string &candidate_file);

/// Step 5 (plan §5.3): receiver type evidence factor.
/// Replaces the directory-heuristic factorReceiverMatch with actual
/// type-based matching. If the reference carries a known receiver_type
/// (e.g. "Box"), this checks whether the candidate's qualified_name
/// contains that type name (e.g. "Box::draw", "Box.draw"). When
/// receiver_type is empty (unknown/dynamic), returns 0.5 (neutral) —
/// neither boosting nor penalizing — rather than the previous directory
/// heuristic that fabricated positive evidence from file paths.
///
/// @param receiver_type   Inferred receiver type from the reference
///                        (empty = unknown/dynamic).
/// @param candidate_qname Candidate's qualified_name from the entity
///                        table (e.g. "Box::draw", "MyClass.method").
/// @param candidate_name  Candidate's bare name (fallback).
/// @param candidate_file  Candidate's file path (fallback for
///                        extracting class prefix from path).
double factorReceiverTypeMatch(const std::string &receiver_type,
			       const std::string &candidate_qname,
			       const std::string &candidate_name,
			       const std::string &candidate_file);

// ── v0.2.5 (perf): receiver-type match with pre-parsed ref-level strings ──
//
// factorReceiverTypeMatch re-derives `receiver_type + "::"`,
// `receiver_type + "."` and the lowercased receiver_type on EVERY candidate.
// All three depend only on the REF's receiver_type (fixed across candidates),
// so in the resolver hot loop we build them once per ref and hand them to a
// pre-parsed variant that skips those allocations. Scoring is IDENTICAL to
// factorReceiverTypeMatch — do not change it independently.
struct ReceiverMatchContext {
	std::string prefix1; // receiver_type + "::"
	std::string prefix2; // receiver_type + "."
	std::string rtype_lower; // lowercased receiver_type
	bool empty = false; // receiver_type was empty (neutral 0.5)
};

/// Build the ref-level context for receiver matching once per reference.
ReceiverMatchContext
buildReceiverMatchContext(const std::string &receiver_type);

/// Same scoring as factorReceiverTypeMatch but uses a pre-parsed context so
/// the per-candidate prefix/prefix2/lowercase allocations are eliminated.
/// candidate_file's lowercased base name is computed inside (per candidate).
double factorReceiverTypeMatchPrecomp(const ReceiverMatchContext &ctx,
				      const std::string &candidate_qname,
				      const std::string &candidate_file);

/// Check if the name is a very common function name that causes
/// high false-positive cross-module matches (e.g. Len, Init, Run).
/// Returns kCommonNamePenaltyValue if the name is in the common list, 0.0 otherwise.
double factorCommonNamePenalty(const std::string &name);

/// Check language-specific visibility rules for cross-module calls.
/// In Go, unexported names (lowercase first letter) cannot be called
/// from another package. In Python, names starting with '_' are private.
/// In Java, package-private names (no 'public' modifier) are restricted.
/// Returns 0.0 if the candidate is NOT visible from the caller's module
/// (i.e. the match is a false positive), 1.0 if visible or unknown.
///
/// Reference: codebase-memory-mcp (MIT, https://github.com/DeusData/codebase-memory-mcp)
///   internal/cbm/helpers.c :: cbm_is_exported()
double factorVisibilityCheck(const std::string &language,
			     const std::string &candidate_name,
			     const std::string &caller_file,
			     const std::string &candidate_file);

/// True if `file_path` ends in a C/C++ translation-unit (source)
/// extension: .c .cpp .cc .cxx .c++ .m .mm
bool isCppSourceFile(const std::string &file_path);

/// True if `file_path` ends in a C/C++ header extension:
/// .h .hpp .hh .hxx .h++ .inl .ipp .tpp .tcc
bool isCppHeaderFile(const std::string &file_path);

/// Definition-priority factor for C/C++.
/// Returns kScoreExactMatch (1.0) for a symbol defined in a C/C++ source
/// file, kScorePenalty (-0.5) for a symbol declared only in a header, and
/// 1.0 (neutral) for non-C/C++ languages or unrecognized extensions.
/// Implements the README promise that .c/.cpp definitions are preferred
/// over .h prototypes, and breaks the arbitrary entity_index insertion-order
/// tie that previously decided C/C++ resolution targets (Finding #8).
double factorDefinitionMatch(const std::string &language,
			     const std::string &candidate_file);

} // namespace resolver

#endif // CODESCOPE_RESOLVER_FACTORS_H