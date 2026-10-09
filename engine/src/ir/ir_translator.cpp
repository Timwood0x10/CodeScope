#include "ir_translator.h"

#include <cstdio>

#include "translators/js_visitor.h"
#include "translators/ts_visitor.h"
#include "translators/tsx_visitor.h"
#include "translators/c_visitor.h"
#include "translators/cpp_visitor.h"
#include "translators/go_visitor.h"
#include "translators/python_visitor.h"
#include "translators/rust_visitor.h"
#include "translators/java_visitor.h"
// Swift has no visitor here: its grammar is not vendored (see
// parser/parser.cpp for the ABI reason), so a .swift file never reaches this
// layer. The Swift visitor/translator sources were removed rather than left
// unreachable — restore them together with the grammar.

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
std::unique_ptr<Translator> createTsxTranslator();
} // namespace ir

namespace ir
{

std::unique_ptr<Translator> createTranslator(const char *language)
{
	// Guard against null language pointer
	if (!language)
		language = "";
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
	// No Swift branch: the grammar is not vendored (parser/parser.cpp) and
	// the translator was removed with it. Swift files are detected by
	// extension and reported as `language_missing` parse failures instead.
	if (lang == "tsx")
		return createTsxTranslator();

	return nullptr;
}

bool TranslateDepth::exceeded(const char *file_path, const char *method)
{
	if (depth_ < kMaxTranslateDepth)
		return false;
	if (!reported_) {
		// Reported once per file: a truncated translation must be visible
		// (plan/rules/code_rules.md, no silent handling), and the message
		// names the module and method so it can be traced back here.
		reported_ = true;
		fprintf(stderr,
			"[module=ir, method=%s] AST nesting exceeded "
			"kMaxTranslateDepth=%d in '%s'; deeper nodes skipped to "
			"avoid stack overflow (legacy translator)\n",
			method ? method : "translateChildren",
			kMaxTranslateDepth, file_path ? file_path : "");
	}
	return true;
}

std::unique_ptr<JsVisitor> createJsVisitor(const char *language)
{
	// Guard against null language pointer
	if (!language)
		language = "";
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
	// No Swift branch: see the note at the top of this file.

	return nullptr;
}

} // namespace ir
