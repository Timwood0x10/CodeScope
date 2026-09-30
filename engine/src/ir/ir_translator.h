#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H

#include "ir.h"
#include <memory>

// tree-sitter types
typedef struct TSTree TSTree;

namespace ir
{

// Forward declaration for the new visitor-based pipeline
class JsVisitor;

// Abstract base for language-specific CST → IR translators.
// Pure function: source tree → IR TranslationUnit.
// No resolver, no graph, no DB — thread-safe, each call is independent.

// ─── Legacy-translator recursion bound ───────────────────────────
// The LIVE IR pipeline is the visitors (`*_visitor.cpp`), which bound their
// recursion with kMaxVisitDepth and report the truncation. The translators
// (`*_translator.cpp`) are the fallback used when no visitor exists for a
// language, and their tree walk is just as recursive: a pathologically deep
// AST would overflow the indexer's 512 KB worker stack, and the FFI try/catch
// cannot recover a stack overflow. They share the visitors' bound so the two
// pipelines cannot drift apart.
constexpr int kMaxTranslateDepth = 250;

/// Per-file recursion counter for a translator.
///
/// Not thread-safe by design: one instance belongs to one translator, and a
/// translator instance is used by a single parse worker at a time.
class TranslateDepth {
    public:
	/// Report whether the walk must stop at the current depth. The first stop
	/// in a file is logged to stderr, tagged with the module and method, so a
	/// truncated translation is never silent; later stops are suppressed to
	/// keep one line per file.
	/// \param file_path  File being translated (for the report).
	/// \param method     Caller name (for the report).
	/// \return true when the caller must return without recursing further.
	bool exceeded(const char *file_path, const char *method);

	/// Enter one level. Paired with pop() (DepthGuard does both).
	void push()
	{
		++depth_;
	}
	/// Leave one level.
	void pop()
	{
		--depth_;
	}
	/// Reset for a new file.
	void reset()
	{
		depth_ = 0;
		reported_ = false;
	}
	/// Current depth (tests and diagnostics).
	int depth() const
	{
		return depth_;
	}

    private:
	int depth_ = 0;
	bool reported_ = false;
};

/// RAII bracket around one recursion level: push on construction, pop on
/// destruction, so an early return inside the walk cannot leave the counter
/// skewed.
class DepthGuard {
    public:
	/// \param depth  Counter owned by the calling translator.
	explicit DepthGuard(TranslateDepth &depth)
		: depth_(depth)
	{
		depth_.push();
	}
	~DepthGuard()
	{
		depth_.pop();
	}
	DepthGuard(const DepthGuard &) = delete;
	DepthGuard &operator=(const DepthGuard &) = delete;

    private:
	TranslateDepth &depth_;
};

class Translator {
    public:
	virtual ~Translator() = default;

	// Translate a tree-sitter CST into an IR TranslationUnit.
	// Ownership of the returned TranslationUnit passes to the caller.
	virtual TranslationUnit *translate(TSTree *tree, const char *source,
					   const char *file_path) = 0;

	virtual const char *language() const = 0;
};

// Factory
std::unique_ptr<Translator> createTranslator(const char *language);

// Create a visitor for JS/TS/TSX languages. Returns nullptr for other langs.
// The returned JsVisitor (or TsVisitor/TsxVisitor) emits SemanticUnit records.
std::unique_ptr<JsVisitor> createJsVisitor(const char *language);

} // namespace ir

#endif // IR_TRANSLATOR_H
