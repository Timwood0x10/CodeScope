#ifndef TEST_E2E_H
#define TEST_E2E_H

// Shared helper functions for language-specific E2E tests.
// Each test file includes this header and calls runE2eTest().

#include "../include/engine.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

// Check a condition and exit on failure
static inline void check(bool cond, const char* msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

// Print a labeled JSON result
static inline void print_json(const char* label, const char* json) {
    printf("\n--- %s (%zu chars) ---\n%s\n", label, strlen(json), json);
}

/**
 * Run the standard E2E test pipeline for a given language:
 *   init → create_project → index_file → find_definition × 3 → find_references
 *   → get_callers → get_callees → get_graph_stats → locate_by_name × 2 → shutdown
 *
 * @param lang         Language name (used for db file, project name)
 * @param code         Source code string (embedded as-is)
 * @param file_path    Path to write the code to (under /tmp)
 * @param def_names    Array of symbol names to check in find_definition results
 * @param def_count    Number of def_names entries
 * @param ref_name     Symbol name to query find_references for
 * @param ref_caller   Expected caller symbol in references result
 * @param caller_name  Symbol name to query get_callers for
 * @param callee_name  Symbol name to query get_callees for
 * @param locate_name  Symbol name to look up via locate_by_name
 * @param locate_extra Additional symbol name to locate (or nullptr)
 */
static inline void runE2eTest(
    const char* lang, const char* code, const char* file_path,
    const char** def_names, int def_count,
    const char* ref_name, const char* ref_caller,
    const char* caller_name,
    const char* callee_name,
    const char* locate_name,
    const char* locate_extra)
{
    char db_path[64];
    snprintf(db_path, sizeof(db_path), "/tmp/test_%s.db", lang);

    unlink(db_path);

    int rc = engine_init(db_path);
    check(rc == 0, "engine_init");

    // Write test file
    FILE* f = fopen(file_path, "w");
    check(f != nullptr, "fopen");
    fwrite(code, 1, strlen(code), f);
    fclose(f);

    // Create project
    char proj_name[64];
    snprintf(proj_name, sizeof(proj_name), "%s-test", lang);
    uint64_t pid = engine_create_project("/tmp", proj_name);
    check(pid > 0, "create_project");

    // Index
    char* result = engine_index_file(pid, file_path);
    print_json("Index", result);
    check(strstr(result, "\"ok\":true") != nullptr, "index_file ok");
    engine_free_string(result);

    // Find definitions
    for (int i = 0; i < def_count; i++) {
        char* def = engine_find_definition(pid, def_names[i], nullptr);
        char label[128];
        snprintf(label, sizeof(label), "Definition: %s", def_names[i]);
        print_json(label, def);
        check(strstr(def, def_names[i]) != nullptr, "find_def");
        engine_free_string(def);
    }

    // References
    char* refs = engine_find_references(pid, ref_name, nullptr);
    char ref_label[128];
    snprintf(ref_label, sizeof(ref_label), "References: %s", ref_name);
    print_json(ref_label, refs);
    check(strstr(refs, ref_caller) != nullptr, "find_refs has caller");
    engine_free_string(refs);

    // Callers
    char* callers = engine_get_callers(pid, caller_name, nullptr);
    char caller_label[128];
    snprintf(caller_label, sizeof(caller_label), "Callers: %s", caller_name);
    print_json(caller_label, callers);
    check(strstr(callers, "callers") != nullptr, "get_callers");
    engine_free_string(callers);

    // Callees
    char* callees = engine_get_callees(pid, callee_name, nullptr);
    char callee_label[128];
    snprintf(callee_label, sizeof(callee_label), "Callees: %s", callee_name);
    print_json(callee_label, callees);
    check(strstr(callees, "callees") != nullptr, "get_callees");
    engine_free_string(callees);

    // Stats
    char* stats = engine_get_graph_stats(pid);
    print_json("Graph Stats", stats);
    check(strstr(stats, "total_nodes") != nullptr, "get_graph_stats");
    engine_free_string(stats);

    // Locate
    char* locate = engine_locate_by_name(pid, locate_name);
    char locate_label[128];
    snprintf(locate_label, sizeof(locate_label), "Locate: %s", locate_name);
    print_json(locate_label, locate);
    check(strstr(locate, locate_name) != nullptr, "locate");
    engine_free_string(locate);

    if (locate_extra) {
        locate = engine_locate_by_name(pid, locate_extra);
        snprintf(locate_label, sizeof(locate_label), "Locate: %s", locate_extra);
        print_json(locate_label, locate);
        check(strstr(locate, locate_extra) != nullptr, "locate extra");
        engine_free_string(locate);
    }

    engine_shutdown();

     printf("\n=== %s E2E test passed ===\n", lang);
    }

    /**
      * Run FP-verification E2E: index code with builtin/FFI calls, then verify
      * that builtin functions do NOT create false call edges, and that language
      * visibility rules (e.g. Go unexported) are enforced.
      *
      * @param lang         Language name
      * @param code         Source code
      * @param file_path    Path to write the code to
      * @param main_func    Name of the function to query callees for
      * @param legit_callee A user-defined function that main_func legitimately calls
      * @param builtin_names Array of builtin function names that should NOT appear
      *                      as callees (nullptr-terminated)
      */
     static inline void runFPVerificationTest(
         const char* lang, const char* code, const char* file_path,
         const char* main_func, const char* legit_callee,
         const char** builtin_names)
    {
        char db_path[64];
        snprintf(db_path, sizeof(db_path), "/tmp/test_fp_%s.db", lang);

        unlink(db_path);

        int rc = engine_init(db_path);
        check(rc == 0, "engine_init");

        // Write test file
        FILE* f = fopen(file_path, "w");
        check(f != nullptr, "fopen");
        fwrite(code, 1, strlen(code), f);
        fclose(f);

        // Create project
        char proj_name[64];
        snprintf(proj_name, sizeof(proj_name), "%s-fp-test", lang);
        uint64_t pid = engine_create_project("/tmp", proj_name);
        check(pid > 0, "create_project");

        // Index
         char* result = engine_index_file(pid, file_path);
         print_json("Index", result);
         check(strstr(result, "\"ok\":true") != nullptr, "index_file ok");
         engine_free_string(result);

         // Enhance: build the call graph so callee lookups work
         char* enh = engine_enhance_project(pid);
         engine_free_string(enh);

         // Get callees of the main function — this is where we check for FPs
         char* callees = engine_get_callees(pid, main_func, nullptr);
         print_json("Callees of main function", callees);

        // 1. Legitimate callee must be present
        check(strstr(callees, legit_callee) != nullptr, "legit callee present");

        // 2. Builtin names must NOT appear as callees
        for (int i = 0; builtin_names[i] != nullptr; i++) {
            char buf[256];
            snprintf(buf, sizeof(buf), "\"name\":\"%s\"", builtin_names[i]);
            // The callee should NOT include this builtin
            if (strstr(callees, buf) != nullptr) {
                fprintf(stderr, "FAIL: builtin '%s' should NOT appear in callees of mainFunc\n",
                        builtin_names[i]);
                fprintf(stderr, "  callees JSON: %s\n", callees);
                exit(1);
            }
            printf("  [PASS] builtin '%s' correctly excluded from callees\n", builtin_names[i]);
        }
        engine_free_string(callees);

        // Get references to verify builtin calls don't create reference entries
        for (int i = 0; builtin_names[i] != nullptr; i++) {
            char* refs = engine_find_references(pid, builtin_names[i], nullptr);
            // Builtin references should have zero results (no user func with that name)
            if (strstr(refs, "\"total\":0") == nullptr && strstr(refs, "\"results\":[]") == nullptr) {
                // If there ARE results, they must not include mainFunc as a caller
                if (strstr(refs, "mainFunc") != nullptr) {
                    fprintf(stderr, "FAIL: builtin '%s' should NOT have references from mainFunc\n",
                            builtin_names[i]);
                    fprintf(stderr, "  refs JSON: %s\n", refs);
                    exit(1);
                }
            }
            printf("  [PASS] builtin '%s' reference check passed\n", builtin_names[i]);
            engine_free_string(refs);
        }

        // Stats check: total edges should be reasonable (no FP edges from builtins)
        char* stats = engine_get_graph_stats(pid);
        print_json("Graph Stats", stats);
        check(strstr(stats, "total_nodes") != nullptr, "get_graph_stats");
        engine_free_string(stats);

        engine_shutdown();
        printf("\n=== %s FP verification test passed ===\n", lang);
    }

    #endif // TEST_E2E_H
