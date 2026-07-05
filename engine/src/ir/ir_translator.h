#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H

#include "ir.h"

// tree-sitter types
typedef struct TSTree TSTree;

// Forward declare resolver interface
namespace resolver
{
class Resolver;
}

namespace ir
{

// Abstract base for language-specific CST → IR translators.
// Each language implements one.

class Translator {
    public:
	virtual ~Translator() = default;

	// Translate a tree-sitter CST into an IR TranslationUnit.
	// Ownership of the returned TranslationUnit passes to the caller.
	virtual TranslationUnit *translate(TSTree *tree, const char *source,
					   const char *file_path) = 0;

	virtual const char *language() const = 0;

	// Set an optional resolver chain for cross-file symbol resolution.
	// Must be called BEFORE translate(). The resolver is NOT owned
	// by the translator — the caller must keep it alive.
	virtual void setResolver(resolver::Resolver *resolver)
	{
		resolver_ = resolver;
	}

    protected:
	resolver::Resolver *resolver_ = nullptr;
};

// Factory
Translator *createTranslator(const char *language);

} // namespace ir

#endif // IR_TRANSLATOR_H
