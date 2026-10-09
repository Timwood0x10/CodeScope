#include "test_e2e.h"

int main()
{
	const char *code = R"(class Calculator {
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

    String describe(int value) {
        return format(value);
    }

    String format(int value) {
        return "v";
    }
}

class Main {
    public static void main(String[] args) {
        Calculator calc = new Calculator();
        int r = calc.compute(5, 3);
    }
}
)";

	// `describe` and `format` return a CLASS type (`String`). This grammar
	// spells a class type `identifier` too, so naming a declaration by its
	// first `identifier` child named such methods after their return type: the
	// entity used to be called `String`, and find_definition("describe") — the
	// assertion below, via runE2eTest's def list — answered nothing. Primitives
	// and `void` never showed it (their own node types), which is why the
	// original three-definition fixture passed.
	const char *defs[] = { "add", "Calculator", "multiply", "describe",
			       "format" };
	runE2eTest("java", code, "/tmp/TestApp.java", defs, 5, "format",
		   "describe", "format", "describe", "main", nullptr);
	return 0;
}
