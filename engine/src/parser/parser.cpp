#include "parser.h"

#include <dlfcn.h>
#include <algorithm>
#include <cstring>
#include <tree_sitter/api.h>

// ── Construction ───────────────────────────────────────────────

Parser::Parser() {}

Parser::~Parser() {
    for (auto &[name, g] : grammars_) {
        if (g.handle)
            dlclose(g.handle);
    }
    grammars_.clear();
}

// ── Language Registration ─────────────────────────────────────

bool Parser::registerLanguage(const char *name, const char *so_path) {
    if (hasLanguage(name))
        return true; // already registered

    void *handle = dlopen(so_path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        error_ = std::string("dlopen failed for ") + so_path + ": " + dlerror();
        return false;
    }

    // Build the symbol name: tree_sitter_<name>
    std::string sym = "tree_sitter_";
    sym += name;

    // Replace hyphens with underscores in symbol name
    std::replace(sym.begin(), sym.end(), '-', '_');

    auto *fn = reinterpret_cast<const TSLanguage *(*)()>(dlsym(handle, sym.c_str()));
    if (!fn) {
        error_ = std::string("dlsym failed for ") + sym + " in " + so_path + ": " + dlerror();
        dlclose(handle);
        return false;
    }

    grammars_[name] = Grammar{handle, fn};
    return true;
}

bool Parser::hasLanguage(const char *name) const {
    return grammars_.count(name) > 0;
}

const TSLanguage *Parser::getLanguage(const char *name) {
    auto it = grammars_.find(name);
    if (it != grammars_.end()) {
        return it->second.fn();
    }
    return nullptr;
}

// ── Parse ─────────────────────────────────────────────────────

TSTree *Parser::parse(const char *file_path, const char *source, const char *language) {
    const TSLanguage *lang = getLanguage(language);
    if (!lang) {
        error_ = std::string("Language not registered: ") + language;
        return nullptr;
    }

    TSParser *ts_parser = ts_parser_new();
    ts_parser_set_language(ts_parser, lang);

    TSTree *tree =
        ts_parser_parse_string(ts_parser, nullptr, source, static_cast<uint32_t>(strlen(source)));

    if (!tree) {
        error_ = std::string("Parse failed for ") + file_path;
    } else {
        error_.clear();
    }

    ts_parser_delete(ts_parser);
    return tree;
}
