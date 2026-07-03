#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

static void check(bool cond, const char* msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

static void print_json(const char* label, const char* json) {
    printf("\n--- %s (%zu chars) ---\n%s\n", label, strlen(json), json);
}

int main() {
    unlink("/tmp/test_rust.db");

    int rc = engine_init("/tmp/test_rust.db");
    check(rc == 0, "engine_init");

    // Write a Rust test file
    const char* code = R"(fn add(a: i32, b: i32) -> i32 {
    a + b
}

fn multiply(a: i32, b: i32) -> i32 {
    let mut result = 0;
    for _ in 0..b {
        result = add(result, a);
    }
    result
}

fn compute(x: i32, y: i32) -> i32 {
    let a = add(x, y);
    let b = add(x, y);
    multiply(a, b)
}

fn main() {
    let r = compute(5, 3);
    println!("{}", r);
}
)";
    const char* file_path = "/tmp/test_math.rs";

    FILE* f = fopen(file_path, "w");
    check(f, "fopen");
    fwrite(code, 1, strlen(code), f);
    fclose(f);

    // Create project
    uint64_t pid = engine_create_project("/tmp", "rust-test");
    check(pid > 0, "create_project");

    // Index
    char* result = engine_index_file(pid, file_path);
    print_json("Index", result);
    check(strstr(result, "\"ok\":true") != nullptr, "index_file ok");
    engine_free_string(result);

    // Find definition: add
    char* def = engine_find_definition(pid, "add", nullptr);
    print_json("Definition: add", def);
    check(strstr(def, "\"name\":\"add\"") != nullptr, "find_def add");
    engine_free_string(def);

    // Find definition: multiply
    def = engine_find_definition(pid, "multiply", nullptr);
    print_json("Definition: multiply", def);
    check(strstr(def, "\"name\":\"multiply\"") != nullptr, "find_def multiply");
    engine_free_string(def);

    // Find definition: compute
    def = engine_find_definition(pid, "compute", nullptr);
    print_json("Definition: compute", def);
    check(strstr(def, "\"name\":\"compute\"") != nullptr, "find_def compute");
    engine_free_string(def);

    // References: add
    char* refs = engine_find_references(pid, "add", nullptr);
    print_json("References: add", refs);
    check(strstr(refs, "multiply") != nullptr || strstr(refs, "compute") != nullptr,
          "find_refs add has callers");
    engine_free_string(refs);

    // Callers: add
    char* callers = engine_get_callers(pid, "add");
    print_json("Callers: add", callers);
    check(strstr(callers, "callers") != nullptr, "get_callers add");
    engine_free_string(callers);

    // Callees: compute
    char* callees = engine_get_callees(pid, "compute");
    print_json("Callees: compute", callees);
    check(strstr(callees, "callees") != nullptr, "get_callees compute");
    engine_free_string(callees);

    // Stats
    char* stats = engine_get_graph_stats(pid);
    print_json("Graph Stats", stats);
    check(strstr(stats, "total_nodes") != nullptr, "get_graph_stats");
    engine_free_string(stats);

    // Locate: main
    char* locate = engine_locate_by_name(pid, "main");
    print_json("Locate: main", locate);
    check(strstr(locate, "\"name\":\"main\"") != nullptr, "locate main");
    engine_free_string(locate);

    engine_shutdown();

    printf("\n=== Rust E2E test passed ===\n");
    return 0;
}
