#pragma once
#include "NxMatrix/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace NxMatrix::Crypto {

ByteArray Sha256(const std::uint8_t *Data, std::size_t Size);
ByteArray Sha256(const ByteArray &Data);
ByteArray Sha256(const std::string &Data);

} // namespace NxMatrix::Crypto