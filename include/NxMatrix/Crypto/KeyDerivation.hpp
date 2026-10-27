#pragma once
#include "NxMatrix/Crypto/Types.hpp"

#include <cstdint>
#include <string>

namespace NxMatrix::Crypto {

// PIN/password -> master key (PBKDF2-HMAC-SHA256).
class KeyDerivation {
public:
    // ~200k iterations: roughly 0.2s on the Switch CPU, fine for a single
    // derivation at unlock time, painful for a brute-force attacker.
    static constexpr std::uint32_t DefaultIterations = 200000;

    static KeyBytes DeriveKey(const std::string &Secret, const SaltBytes &Salt, std::uint32_t Iterations);
};

} // namespace NxMatrix::Crypto