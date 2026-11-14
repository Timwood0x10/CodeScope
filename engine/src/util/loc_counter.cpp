#include "loc_counter.h"

#include <cctype>

namespace util
{

namespace
{

bool isBlankLine(const std::string &line)
{
	for (char c : line)
		if (!std::isspace(static_cast<unsigned char>(c)))
			return false;
	return true;
}

bool isCommentLine(const std::string &line)
{
	for (char c : line) {
		if (std::isspace(static_cast<unsigned char>(c)))
			continue;
		return c == '/' || c == '#';
	}
	return false;
}

} // namespace

LocBreakdown countLoc(const std::string &source)
{
	LocBreakdown result;
	std::string line;
	for (size_t i = 0; i <= source.size(); ++i) {
		if (i == source.size()) {
			// Last line without trailing newline
			if (!line.empty()) {
				++result.loc;
				if (isBlankLine(line))
					++result.blank_loc;
				else if (isCommentLine(line))
					++result.comment_loc;
				else
					++result.code_loc;
			}
			break;
		}
		if (source[i] == '\n') {
			++result.loc;
			if (isBlankLine(line))
				++result.blank_loc;
			else if (isCommentLine(line))
				++result.comment_loc;
			else
				++result.code_loc;
			line.clear();
		} else if (source[i] != '\r') {
			line += source[i];
		}
	}
	return result;
}

} // namespace util
