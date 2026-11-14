#ifndef CODESCOPE_UTIL_LOC_COUNTER_H
#define CODESCOPE_UTIL_LOC_COUNTER_H

#include <string>

namespace util
{

/// Line-of-code breakdown for a source file.
/// `loc` is the number of newline-delimited lines: a file with "a\nb\n" has
/// 2, a file with "a\nb" has 2, and an empty file has 0. A trailing newline
/// therefore does not add a line, which matches `wc -l` for the usual case.
/// `code_loc`, `comment_loc` and `blank_loc` partition those lines by a simple
/// heuristic: a line whose first non-whitespace character is `/` or `#` is a
/// comment line; a line with only whitespace is blank; everything else is
/// code.
/// This is deliberately crude — it does not track block-comment
/// state — because the goal is a useful aggregate, not a per-line
/// verdict. The partition is exact: code + comment + blank == loc.
struct LocBreakdown {
	int loc = 0;
	int code_loc = 0;
	int comment_loc = 0;
	int blank_loc = 0;
};

LocBreakdown countLoc(const std::string &source);

} // namespace util

#endif // CODESCOPE_UTIL_LOC_COUNTER_H
