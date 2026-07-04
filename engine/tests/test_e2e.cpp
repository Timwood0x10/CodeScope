#include "../include/engine.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cassert>

// Write a Python test file and run the full pipeline:
// parse → IR → graph → store → query

static void writeTestFile(const char* path) {
    std::ofstream f(path);
    f << R"PY(
# calculator.py — A simple calculator module

def add(a, b):
    """Add two numbers."""
    return a + b

def subtract(a, b):
    """Subtract b from a."""
    return a - b

def multiply(a, b):
    """Multiply two numbers."""
    result = 0
    for i in range(b):
        result = add(result, a)
    return result

class Calculator:
    """A calculator class."""

    def __init__(self, name):
        self.name = name
        self.total = 0

    def add_value(self, value):
        self.total = add(self.total, value)

    def compute(self, x, y):
        a = add(x, y)
        b = subtract(x, y)
        return multiply(a, b)

def main():
    calc = Calculator("test")
    calc.add_value(10)
    result = calc.compute(5, 3)
    print(result)

if __name__ == "__main__":
    main()
)PY";
    f.close();
}

static void printJson(const char* label, const char* json) {
    printf("=== %s ===\n%s\n\n", label, json);
}

int main() {
    const char* db_path = "/tmp/astgraph_test.db";
    const char* test_file = "/tmp/calculator.py";

    // Clean slate
    remove(db_path);

    // Write test file
    writeTestFile(test_file);
    printf("Test file written: %s\n\n", test_file);

    // ── Init engine ────────────────────────────────────────────
    int rc = engine_init(db_path);
    assert(rc == 0);
    printf("[OK] Engine initialized\n");

    // ── Create project ─────────────────────────────────────────
    uint64_t pid = engine_create_project("/tmp", "test-project");
    assert(pid > 0);
    printf("[OK] Project created: id=%llu\n\n", (unsigned long long)pid);

    // ── Index file ─────────────────────────────────────────────
    char* result = engine_index_file(pid, test_file);
    printf(">>> Index result:\n%s\n\n", result);
    assert(strstr(result, "\"ok\":true") != nullptr);
    engine_free_string(result);

    // ── Get graph stats ────────────────────────────────────────
    result = engine_get_graph_stats(pid);
    printJson("GRAPH STATS", result);
    engine_free_string(result);

    // ── Find definition: add ───────────────────────────────────
    result = engine_find_definition(pid, "add", nullptr);
    printJson("FIND DEFINITION 'add'", result);
    {
        // Should find add function at line ~4
        assert(strstr(result, "\"name\":\"add\"") != nullptr);
        assert(strstr(result, "\"file_path\":\"/tmp/calculator.py\"") != nullptr);
        const char* total = strstr(result, "\"total\"");
        assert(total != nullptr);
    }
    engine_free_string(result);

    // ── Find definition: Calculator ────────────────────────────
    result = engine_find_definition(pid, "Calculator", nullptr);
    printJson("FIND DEFINITION 'Calculator'", result);
    assert(strstr(result, "\"name\":\"Calculator\"") != nullptr);
    engine_free_string(result);

    // ── Find references: add ───────────────────────────────────
    result = engine_find_references(pid, "add", nullptr);
    printJson("FIND REFERENCES 'add'", result);
    // multiply calls add(result, a) and compute calls add(x, y) and add_value calls add
    assert(strstr(result, "multiply") != nullptr || strstr(result, "compute") != nullptr);
    engine_free_string(result);

    // ── Get callers: add ───────────────────────────────────────
    result = engine_get_callers(pid, "add");
    printJson("GET CALLERS 'add'", result);
    engine_free_string(result);

    // ── Get callees: multiply ──────────────────────────────────
    result = engine_get_callees(pid, "multiply");
    printJson("GET CALLEES 'multiply'", result);
    engine_free_string(result);

    // ── Locate by name ─────────────────────────────────────────
    result = engine_locate_by_name(pid, "main");
    printJson("LOCATE 'main'", result);
    engine_free_string(result);

    result = engine_locate_by_name(pid, "Calculator");
    printJson("LOCATE 'Calculator'", result);
    engine_free_string(result);

    // ── Summary ────────────────────────────────────────────────
    printf("\n====================\n");
    printf("ALL TESTS PASSED\n");
    printf("====================\n");

    engine_shutdown();
    return 0;
}
