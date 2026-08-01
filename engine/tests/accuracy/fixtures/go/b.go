package main

// bravo is a package-level function in a second file. alpha (a.go)
// calls it — a cross-file same-package bare-name call.
func bravo(x int) int {
	return x
}

// helper is a same-name function in b.go. a.go does NOT call it; it
// exists to test that the resolver does not cross-wire homonyms.
func helper() int {
	return 42
}
