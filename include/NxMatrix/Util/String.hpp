#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace NxMatrix::Util {

std::string ToLower(const std::string &Text);
std::string Trim(const std::string &Text);
bool StartsWith(const std::string &Text, const std::string &Prefix);
bool EndsWith(const std::string &Text, const std::string &Suffix);
std::vector<std::string> Split(const std::string &Text, char Separator);
std::string Join(const std::vector<std::string> &Parts, const std::string &Separator);
std::string ReplaceAll(std::string Text, const std::string &From, const std::string &To);
std::string UrlEncode(const std::string &Text);

// Shortens to MaxChars characters (UTF-8 safe), appending "..." when cut.
std::string TruncateWithEllipsis(const std::string &Text, std::size_t MaxChars);

} // namespace NxMatrix::Util