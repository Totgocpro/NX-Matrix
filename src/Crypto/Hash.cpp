#include "NxMatrix/Crypto/Hash.hpp"

#include <mbedtls/sha256.h>

namespace NxMatrix::Crypto {

ByteArray Sha256(const std::uint8_t *Data, std::size_t Size) {
    ByteArray Digest(32, 0);
    // mbedtls rejects a null input even for length 0; keep a stable pointer.
    static const std::uint8_t kEmpty = 0;
    const std::uint8_t *Input = (Data != nullptr && Size != 0) ? Data : &kEmpty;
    mbedtls_sha256_ret(Input, Size, Digest.data(), 0);
    return Digest;
}

ByteArray Sha256(const ByteArray &Data) {
    return Sha256(Data.data(), Data.size());
}

ByteArray Sha256(const std::string &Data) {
    return Sha256(reinterpret_cast<const std::uint8_t *>(Data.data()), Data.size());
}

} // namespace NxMatrix::Crypto