#include "ir_translator.h"

#include "translators/js_visitor.h"
#include "translators/ts_visitor.h"
#include "translators/tsx_visitor.h"

// Forward-declare concrete translators (implemented in translators/ dir)
// Each returns a new Translator* or nullptr if the grammar can't be loaded.

namespace ir
{
// Defined in each translator's .cpp file
Translator *createPythonTranslator();
Translator *createCppTranslator();
Translator *createCTranslator();
Translator *createRustTranslator();
Translator *createJavascriptTranslator();
Translator *createTypescriptTranslator();
Translator *createGoTranslator();
Translator *createJavaTranslator();
Translator *createSwiftTranslator();
Translator *createTsxTranslator();
} // namespace ir

namespace ir
{

Translator *createTranslator(const char *language)
{
	// Normalize language string
	std::string lang(language);
	for (auto &c : lang)
		c = static_cast<char>(std::tolower(c));

	if (lang == "python" || lang == "py")
		return createPythonTranslator();
	if (lang == "cpp" || lang == "c++" || lang == "cxx")
		return createCppTranslator();
	if (lang == "c")
		return createCTranslator();
	if (lang == "rust" || lang == "rs")
		return createRustTranslator();
	if (lang == "javascript" || lang == "js")
		return createJavascriptTranslator();
	if (lang == "typescript" || lang == "ts")
		return createTypescriptTranslator();
	if (lang == "go" || lang == "golang")
		return createGoTranslator();
	if (lang == "java")
		return createJavaTranslator();
	if (lang == "swift")
		return createSwiftTranslator();
	if (lang == "tsx")
		return createTsxTranslator();

	return nullptr;
}

JsVisitor *createJsVisitor(const char *language)
{
	std::string lang(language);
	for (auto &c : lang)
		c = static_cast<char>(std::tolower(c));

	if (lang == "javascript" || lang == "js")
		return new JsVisitor();
	if (lang == "typescript" || lang == "ts")
		return new TsVisitor();
	if (lang == "tsx")
		return new TsxVisitor();

	return nullptr;
}

} // namespace ir
