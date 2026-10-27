#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/Types.hpp"

namespace NxMatrix::Crypto {

// AES-256-CTR as used by Matrix encrypted attachments (JWK alg "A256CTR"):
// the 16-byte IV is the first counter block, then the whole block increments
// big-endian. Unauthenticated: callers verify the ciphertext SHA-256 first.
bool AesCtr256Crypt(const KeyBytes &Key, const std::array<std::uint8_t, 16> &Counter, const ByteArray &In, ByteArray &Out);

} // namespace NxMatrix::Crypto