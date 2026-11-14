// test_loc_counter.cpp — measured file size (D1 §5.1).
//
// The briefing reports `scale.loc` and the file_size_limit rule measures
// `files.loc`. Both are only as good as this counter, and the counter is the
// one place where "how many lines is this file" is decided, so its edges are
// pinned here rather than inferred from a rule's output:
//
//   1. an empty file has 0 lines (not 1);
//   2. a trailing newline does not add a line, a missing one does not lose it;
//   3. CRLF input is counted once per line, not once per byte;
//   4. the partition is exact: code + comment + blank == loc;
//   5. blank lines, `//`, `#` and indented comments are classified.

#include "../src/util/loc_counter.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void expect(const std::string &source, int loc, int code, int comment,
		   int blank, const char *label)
{
	const util::LocBreakdown b = util::countLoc(source);
	if (b.loc != loc || b.code_loc != code || b.comment_loc != comment ||
	    b.blank_loc != blank) {
		std::fprintf(stderr,
			     "FAIL %s: loc=%d code=%d comment=%d blank=%d "
			     "(expected %d/%d/%d/%d)\n",
			     label, b.loc, b.code_loc, b.comment_loc,
			     b.blank_loc, loc, code, comment, blank);
		std::exit(1);
	}
	// The partition must stay exact for every input.
	if (b.code_loc + b.comment_loc + b.blank_loc != b.loc) {
		std::fprintf(stderr, "FAIL %s: partition does not add up\n",
			     label);
		std::exit(1);
	}
}

int main()
{
	expect("", 0, 0, 0, 0, "empty file");
	expect("\n", 1, 0, 0, 1, "single newline");
	expect("a", 1, 1, 0, 0, "one line, no newline");
	expect("a\n", 1, 1, 0, 0, "one line, trailing newline");
	expect("a\nb", 2, 2, 0, 0, "no trailing newline");
	expect("a\nb\n", 2, 2, 0, 0, "trailing newline");
	expect("a\r\nb\r\n", 2, 2, 0, 0, "CRLF");
	expect("\n\n\n", 3, 0, 0, 3, "blank lines");
	expect("  \n\t\n", 2, 0, 0, 2, "whitespace-only lines");
	expect("// c\n# c\n  /* c */\nint a;\n", 4, 1, 3, 0,
	       "comments and code");
	expect("int a;\n\n// note\n", 3, 1, 1, 1, "mixed");
	expect("x = 1; // trailing comment\n", 1, 1, 0, 0,
	       "trailing comment counts as code");

	std::printf("test_loc_counter: all tests passed\n");
	return 0;
}
