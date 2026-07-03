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
    // Clean previous db
    unlink("/tmp/test_go.db");

    int rc = engine_init("/tmp/test_go.db");
    check(rc == 0, "engine_init");

    // Write a Go test file
    const char* code = R"(package main

import "fmt"

type Calculator struct {
    Result int
}

func (c *Calculator) Add(a, b int) int {
    return a + b
}

func (c *Calculator) Subtract(a, b int) int {
    return a - b
}

func NewCalculator() *Calculator {
    return &Calculator{}
}

func main() {
    calc := NewCalculator()
    calc.Add(1, 2)
    result := calc.Subtract(10, 3)
    fmt.Println(result)
}
)";
    const char* file_path = "/tmp/calculator.go";

    FILE* f = fopen(file_path, "w");
    check(f, "fopen");
    fwrite(code, 1, strlen(code), f);
    fclose(f);

    // Create project
    uint64_t pid = engine_create_project("/tmp", "go-test");
    check(pid > 0, "create_project");

    // Index the Go file
    char* result1 = engine_index_file(pid, file_path);
    print_json("Index", result1);
    check(strstr(result1, "\"ok\":true") != nullptr, "index_file ok");
    engine_free_string(result1);

    // Define
    char* def = engine_find_definition(pid, "Calculator", nullptr);
    print_json("Definition: Calculator", def);
    check(strstr(def, "Calculator") != nullptr, "find_def Calculator");
    engine_free_string(def);

    def = engine_find_definition(pid, "Add", nullptr);
    print_json("Definition: Add", def);
    check(strstr(def, "Add") != nullptr, "find_def Add");
    engine_free_string(def);

    // References
    char* refs = engine_find_references(pid, "Add", nullptr);
    print_json("References: Add", refs);
    check(strstr(refs, "total") != nullptr, "find_refs Add");
    engine_free_string(refs);

    // Callers
    char* callers = engine_get_callers(pid, "Add");
    print_json("Callers: Add", callers);
    check(strstr(callers, "callers") != nullptr, "get_callers Add");
    engine_free_string(callers);

    // Callees
    char* callees = engine_get_callees(pid, "main");
    print_json("Callees: main", callees);
    check(strstr(callees, "callees") != nullptr, "get_callees main");
    engine_free_string(callees);

    // Stats
    char* stats = engine_get_graph_stats(pid);
    print_json("Graph Stats", stats);
    engine_free_string(stats);

    // Locate
    char* locate = engine_locate_by_name(pid, "main");
    print_json("Locate: main", locate);
    check(strstr(locate, "main") != nullptr, "locate main");
    engine_free_string(locate);

    engine_shutdown();

    printf("\n=== Go E2E test passed ===\n");
    return 0;
}
