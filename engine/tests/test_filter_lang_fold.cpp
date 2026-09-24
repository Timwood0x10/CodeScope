// test_filter_lang_fold.cpp — language-filter alias folding and token
// trimming. Regression cover for: (a) a "c" filter must accept files the
// classifier labels "cpp" and the reverse (the two labels disagree for
// `.c`); (b) whitespace/CRLF-tainted filter tokens must fold like the bare
// label instead of silently matching nothing.

#include "filter_policy.h"

#include <cstdio>

namespace {

int fail(const char *msg)
{
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return 1;
}

} // namespace

int main()
{
	// Alias family: "c" accepts c-family labels both ways.
	FilterPolicy p;
	p.setLanguageFilter("c");
	if (!p.isLanguageAccepted("c"))
		return fail("filter=c must accept label c");
	if (!p.isLanguageAccepted("cpp"))
		return fail("filter=c must accept label cpp (alias family)");
	if (p.isLanguageAccepted("rust"))
		return fail("filter=c must NOT accept label rust");
	if (p.isLanguageAccepted(""))
		return fail("filter=c must NOT accept empty label");

	FilterPolicy r;
	r.setLanguageFilter("cpp");
	if (!r.isLanguageAccepted("c"))
		return fail("filter=cpp must accept label c (alias family)");

	// Token hygiene: CRLF/whitespace-tainted tokens fold like the bare
	// label; a stray "\\r\\n" must not make the filter match nothing.
	FilterPolicy q;
	q.setLanguageFilter("c\r\n, python ");
	if (!q.isLanguageAccepted("c"))
		return fail("CRLF-tainted token must match label c");
	if (!q.isLanguageAccepted("cpp"))
		return fail("CRLF-tainted token must match label cpp");
	if (!q.isLanguageAccepted("python"))
		return fail("space-padded token must match label python");
	if (q.isLanguageAccepted("java"))
		return fail("filter=c,python must NOT accept label java");

	std::printf("=== test_filter_lang_fold PASSED ===\n");
	return 0;
}
