#pragma once
#include <pu/Plutonium>

#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Greedy word wrapping measured with the Plutonium font metrics.
std::vector<std::string> WrapText(const std::string &FontName, const std::string &Text, std::int32_t MaxWidth);
std::int32_t MeasureWrappedHeight(const std::string &FontName, const std::string &Text, std::int32_t MaxWidth);

} // namespace NxMatrix::Ui