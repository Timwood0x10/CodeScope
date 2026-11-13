// template_format.cpp — `{placeholder}` substitution for rule output templates.

#include "template_format.h"

#include <string>
#include <unordered_map>

namespace evidence
{

std::string
formatTemplate(const std::string &tmpl,
	       const std::unordered_map<std::string, std::string> &vars)
{
	std::string out;
	out.reserve(tmpl.size() + 16);
	for (size_t i = 0; i < tmpl.size();) {
		if (tmpl[i] == '{') {
			size_t end = tmpl.find('}', i + 1);
			if (end == std::string::npos) {
				out += tmpl[i++];
				continue;
			}
			std::string key = tmpl.substr(i + 1, end - i - 1);
			auto it = vars.find(key);
			if (it != vars.end())
				out += it->second;
			else
				out += tmpl.substr(i, end - i + 1);
			i = end + 1;
		} else {
			out += tmpl[i++];
		}
	}
	return out;
}

} // namespace evidence
