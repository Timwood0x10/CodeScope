#pragma once

#include <string>

namespace store
{

// Escape a string for safe embedding in JSON.
// Shared across store_*.cpp split files.
std::string jsonEscape(const std::string &s);

} // namespace store
