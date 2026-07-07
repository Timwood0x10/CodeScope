#include "ir_translator.h"

#include "translators/js_visitor.h"
#include "translators/ts_visitor.h"
#include "translators/tsx_visitor.h"
#include "translators/c_visitor.h"
#include "translators/cpp_visitor.h"
#include "translators/go_visitor.h"
#include "translators/python_visitor.h"
#include "translators/rust_visitor.h"
#include "translators/java_visitor.h"
#include "translators/swift_visitor.h"

// Forward-declare concrete translators (implemented in translators/ dir)
// Each returns a new Translator* or nullptr if the grammar can't be loaded.

namespace ir
{
// Defined in each translator's .cpp file
std::unique_ptr<Translator> createPythonTranslator();
std::unique_ptr<Translator> createCppTranslator();
std::unique_ptr<Translator> createCTranslator();
std::unique_ptr<Translator> createRustTranslator();
std::unique_ptr<Translator> createJavascriptTranslator();
std::unique_ptr<Translator> createTypescriptTranslator();
std::unique_ptr<Translator> createGoTranslator();
std::unique_ptr<Translator> createJavaTranslator();
std::unique_ptr<Translator> createSwiftTranslator();
std::unique_ptr<Translator> createTsxTranslator();
} // namespace ir

namespace ir
{

std::unique_ptr<Translator> createTranslator(const char *language)
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

std::unique_ptr<JsVisitor> createJsVisitor(const char *language)
{
	std::string lang(language);
	for (auto &c : lang)
		c = static_cast<char>(std::tolower(c));

	if (lang == "javascript" || lang == "js")
		return std::make_unique<JsVisitor>();
	if (lang == "typescript" || lang == "ts")
		return std::make_unique<TsVisitor>();
	if (lang == "tsx")
		return std::make_unique<TsxVisitor>();

	if (lang == "c")
		return std::make_unique<CVisitor>();
	if (lang == "cpp" || lang == "c++" || lang == "cxx")
		return std::make_unique<CppVisitor>();
	if (lang == "go" || lang == "golang")
		return std::make_unique<GoVisitor>();
	if (lang == "python" || lang == "py")
		return std::make_unique<PythonVisitor>();
	if (lang == "rust" || lang == "rs")
		return std::make_unique<RustVisitor>();
	if (lang == "java")
		return std::make_unique<JavaVisitor>();
	if (lang == "swift")
		return std::make_unique<SwiftVisitor>();

	return nullptr;
}

} // namespace ir
