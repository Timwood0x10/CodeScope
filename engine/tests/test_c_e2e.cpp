#include "test_e2e.h"

int main() {
    const char* code = R"(int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    int result = 0;
    for (int i = 0; i < b; i++) {
        result = add(result, a);
    }
    return result;
}

int compute(int x, int y) {
    int a = add(x, y);
    int b = add(x, y);
    return multiply(a, b);
}

int main() {
    int r = compute(5, 3);
    return r;
}
)";

    const char* defs[] = {"add", "multiply", "compute"};
    runE2eTest("c", code, "/tmp/test_math.c",
               defs, 3,
               "add", "multiply",
               "add",
               "compute",
               "main", nullptr);
    return 0;
}
