#include "ir_translator.h"

// Forward-declare concrete translators (implemented in translators/ dir)
// Each returns a new Translator* or nullptr if the grammar can't be loaded.

namespace ir {
    // Defined in each translator's .cpp file
    Translator* createPythonTranslator();
    Translator* createCppTranslator();
    Translator* createCTranslator();
    Translator* createRustTranslator();
    Translator* createJavascriptTranslator();
    Translator* createTypescriptTranslator();
    Translator* createGoTranslator();
    Translator* createJavaTranslator();
}

namespace ir {

Translator* createTranslator(const char* language) {
    // Normalize language string
    std::string lang(language);
    for (auto& c : lang) c = static_cast<char>(std::tolower(c));

    if (lang == "python" || lang == "py")         return createPythonTranslator();
    if (lang == "cpp" || lang == "c++" || lang == "cxx") return createCppTranslator();
    if (lang == "c")                              return createCTranslator();
    if (lang == "rust" || lang == "rs")           return createRustTranslator();
    if (lang == "javascript" || lang == "js")     return createJavascriptTranslator();
    if (lang == "typescript" || lang == "ts")     return createTypescriptTranslator();
    if (lang == "go" || lang == "golang")         return createGoTranslator();
    if (lang == "java")                           return createJavaTranslator();

    return nullptr;
}

} // namespace ir
