#include "test_e2e.h"

int main() {
    const char* code = R"(class Calculator {
    int add(int a, int b) {
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
}

class Main {
    public static void main(String[] args) {
        Calculator calc = new Calculator();
        int r = calc.compute(5, 3);
    }
}
)";

    const char* defs[] = {"add", "Calculator", "multiply"};
    runE2eTest("java", code, "/tmp/TestApp.java",
               defs, 3,
               "add", "multiply",
               "add",
               "compute",
               "main", nullptr);
    return 0;
}
