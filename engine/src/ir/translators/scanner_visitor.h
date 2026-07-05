#ifndef SCANNER_VISITOR_H
#define SCANNER_VISITOR_H

#include <string>
#include <vector>
#include "../semantic_emitter.h"
#include "../semantic_unit.h"

namespace ir
{

/**
 * Fast scanner visitor — line-based declaration extraction without tree-sitter.
 *
 * For each source file, reads lines and detects function/class/struct/enum/etc.
 * declarations by keyword matching (no parse tree). ~10x faster than tree-sitter.
 *
 * Emits SemanticUnit records compatible with the pipeline, so the same
 * GraphBuilder → Store path works for both fast and full mode.
 *
 * Supported languages: C, C++, Go, Rust, Python, Java, JavaScript, TypeScript,
 * Swift.
 */
class ScannerVisitor {
    public:
	ScannerVisitor();

	/**
     * Scan a source file and produce a SemanticUnit with declaration records.
     * Only top-level declarations are extracted (no function bodies).
     * Returns nullptr on empty/error.
     */
	SemanticUnit *scan(const std::string &source, const char *file_path);

	/** Set the language for interpretation. */
	void setLanguage(const std::string &lang)
	{
		language_ = lang;
	}

    private:
	SemanticUnit *unit_ = nullptr;
	SemanticEmitter *emitter_ = nullptr;
	std::string language_;

	// Helpers
	static std::string_view trimLeft(std::string_view s);
	static bool startsWithKW(std::string_view line, const char *kw);

	/** Try to detect declaration on a line. Returns empty string if none. */
	std::string detectDeclLine(std::string_view line);

	/** Extract name from a declaration line. */
	std::string extractDeclName(std::string_view line,
				    const std::string &kind);

	/** Emit a record for the detected declaration. */
	void emitDecl(const std::string &kind, const std::string &name,
		      SourceRange loc, uint64_t parent_id);
};

} // namespace ir
#endif
