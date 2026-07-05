#ifndef CODESCOPE_GRAMMARS_H
#define CODESCOPE_GRAMMARS_H

struct TSLanguage;

// All vendored tree-sitter grammar init functions (C linkage — compiled as C sources).
// These are compiled into the binary — no dlopen, no .so loading.
#ifdef __cplusplus
extern "C" {
#endif

const TSLanguage *tree_sitter_c();
const TSLanguage *tree_sitter_cpp();
const TSLanguage *tree_sitter_go();
const TSLanguage *tree_sitter_java();
const TSLanguage *tree_sitter_javascript();
const TSLanguage *tree_sitter_python();
const TSLanguage *tree_sitter_rust();
const TSLanguage *tree_sitter_swift();
const TSLanguage *tree_sitter_typescript();
const TSLanguage *tree_sitter_tsx();

#ifdef __cplusplus
}
#endif

#endif // CODESCOPE_GRAMMARS_H
