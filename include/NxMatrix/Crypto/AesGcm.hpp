#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/Types.hpp"

namespace NxMatrix::Crypto {

// AES-256-GCM sealed blob: a fresh random nonce is generated per encryption.
struct SealedData {
    NonceBytes Nonce{};
    TagBytes Tag{};
    ByteArray Ciphertext;
};

class AesGcm {
public:
    // AdditionalData is authenticated but stored outside the blob by the caller.
    static bool Encrypt(const KeyBytes &Key, const ByteArray &Plain, const ByteArray &AdditionalData, SealedData &Out);
    // Returns false on wrong key or tampered data (GCM auth failure).
    static bool Decrypt(const KeyBytes &Key, const SealedData &In, const ByteArray &AdditionalData, ByteArray &OutPlain);
};

} // namespace NxMatrix::Crypto