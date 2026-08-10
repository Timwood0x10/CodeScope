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

// ── Interface dispatch fixture ─────────────────────────────────
// Formatter is an interface; Box implements it via method set
// (Format() below). useFormatterLocal calls f.Format() where f is a
// Formatter-typed variable — the Resolver's cross-file dispatch
// expansion should produce a dispatch edge to Box.Format.
type Formatter interface {
	Format() string
}

// Format makes Box implement Formatter (implicit interface satisfaction).
func (b Box) Format() string {
	return "box"
}

// useFormatterLocal calls the interface method through an interface
// variable — dispatch edge expected: useFormatterLocal -> Format.
func useFormatterLocal() string {
	var f Formatter = Box{val: 5}
	return f.Format()
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
