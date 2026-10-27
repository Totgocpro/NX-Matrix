#pragma once
#include <array>
#include <cstdint>

namespace NxMatrix::Crypto {

using KeyBytes = std::array<std::uint8_t, 32>;   // AES-256 / HKDF / pickle key
using SaltBytes = std::array<std::uint8_t, 16>;  // PBKDF2 salt
using NonceBytes = std::array<std::uint8_t, 12>; // AES-GCM IV
using TagBytes = std::array<std::uint8_t, 16>;   // AES-GCM auth tag

} // namespace NxMatrix::Crypto