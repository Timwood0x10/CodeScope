#include "test_e2e.h"

int main() {
    // Java FP verification: common JDK methods
    // NOTE: use a standalone public class with a main method, not nested classes,
    // for reliable call-edge detection in the old API translator.
    const char* code = R"JAVA(
class Calculator {
    public int userFunction(int x) {
        return x * 2;
    }

    public int mainFunc() {
        int r1 = userFunction(5);

        // Common JDK methods that should NOT create call edges:
        String s = Integer.toString(r1);
        int len = s.length();
        char c = s.charAt(0);

        return r1;
    }
}
)JAVA";

    const char* builtins[] = {
        "toString", "length", "charAt",
        nullptr
    };

    runFPVerificationTest("java", code, "/tmp/Calculator.java",
                          "mainFunc", "userFunction", builtins);
    return 0;
}