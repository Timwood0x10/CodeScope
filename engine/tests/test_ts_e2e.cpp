#include "test_e2e.h"

int main() {
    const char* code = R"(function add(a: number, b: number): number {
    return a + b;
}

function multiply(a: number, b: number): number {
    let result = 0;
    for (let i = 0; i < b; i++) {
        result = add(result, a);
    }
    return result;
}

function compute(x: number, y: number): number {
    const a = add(x, y);
    const b = add(x, y);
    return multiply(a, b);
}

function main(): void {
    const r = compute(5, 3);
    console.log(r);
}

main();
)";

    const char* defs[] = {"add", "multiply", "compute"};
    runE2eTest("ts", code, "/tmp/test_math.ts",
               defs, 3,
               "add", "multiply",
               "add",
               "compute",
               "main", nullptr);
    return 0;
}
