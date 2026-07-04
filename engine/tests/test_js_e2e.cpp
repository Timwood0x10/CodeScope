#include "test_e2e.h"

int main() {
    const char* code = R"(function add(a, b) {
    return a + b;
}

function multiply(a, b) {
    let result = 0;
    for (let i = 0; i < b; i++) {
        result = add(result, a);
    }
    return result;
}

function compute(x, y) {
    const a = add(x, y);
    const b = add(x, y);
    return multiply(a, b);
}

function main() {
    const r = compute(5, 3);
    console.log(r);
}

main();
)";

    const char* defs[] = {"add", "multiply", "compute"};
    runE2eTest("js", code, "/tmp/test_math.js",
               defs, 3,
               "add", "multiply",
               "add",
               "compute",
               "main", nullptr);
    return 0;
}
