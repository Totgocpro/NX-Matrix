#pragma once
#include "NxMatrix/Types.hpp"

#include <cstddef>
#include <cstdint>

namespace NxMatrix::Crypto {

// Cryptographically secure random bytes (Switch csrng service).
void FillRandom(std::uint8_t *Out, std::size_t Size);
ByteArray RandomBytes(std::size_t Size);

} // namespace NxMatrix::Crypto