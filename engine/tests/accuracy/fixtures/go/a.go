package main

import "fmt"

// Box is a struct with a method — used to test receiver method calls.
type Box struct {
	val int
}

// Get is a method on Box. mainFunc calls b.Get() — a selector call.
func (b Box) Get() int {
	return b.val
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
