#include "test_e2e.h"

int main() {
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

    const char* defs[] = {"Calculator", "Add"};
    runE2eTest("go", code, "/tmp/calculator.go",
               defs, 2,
               "Add", "main",
               "Add",
               "main",
               "main", nullptr);
    return 0;
}
