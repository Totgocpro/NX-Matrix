#pragma once
#include <cstdint>
#include <vector>

namespace NxMatrix {

// Shared byte buffer type (media payloads, ciphertext, pickles...).
using ByteArray = std::vector<std::uint8_t>;

} // namespace NxMatrix