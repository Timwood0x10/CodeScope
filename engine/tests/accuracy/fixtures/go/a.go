package main

import "fmt"

// Box is a struct with a method — used to test receiver method calls.
type Box struct {
	val int
}

// double is a method on Box. Get calls b.double() — a selector call
// INSIDE a method body (regression: method bodies were previously not
// walked, so method-internal selector calls were never extracted).
func (b Box) double() int {
	return b.val * 2
}

// Get is a method on Box. mainFunc calls b.Get() — a selector call
// from a function body; Get itself calls b.double() from a method body.
func (b Box) Get() int {
	return b.double()
}

// alpha calls bravo (cross-file, same package, bare name).
func alpha(x int) int {
	return bravo(x)
}

// mainFunc calls alpha (intra-file bare name), len (builtin), and
// b.Get() (method call). It also calls fmt.Println (stdlib, selector).
func mainFunc() {
	s := []int{1, 2}
	_ = alpha(1)
	_ = len(s)
	b := Box{val: 5}
	_ = b.Get()
	fmt.Println(s)
}
