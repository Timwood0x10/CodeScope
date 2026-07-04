#include "test_e2e.h"

int main() {
    const char* code = R"PY(
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

    const char* defs[] = {"add", "Calculator", nullptr};
    runE2eTest("python", code, "/tmp/calculator.py",
               defs, 2,
               "add", "multiply",
               "add",
               "multiply",
               "main", "Calculator");
    return 0;
}
