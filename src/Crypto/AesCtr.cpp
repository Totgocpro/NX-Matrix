#include "NxMatrix/Crypto/AesCtr.hpp"

#include <mbedtls/aes.h>

namespace NxMatrix::Crypto {

bool AesCtr256Crypt(const KeyBytes &Key, const std::array<std::uint8_t, 16> &Counter, const ByteArray &In, ByteArray &Out) {
    mbedtls_aes_context Ctx;
    mbedtls_aes_init(&Ctx);

    // CTR is a stream cipher: setkey_enc (the encryption schedule drives both
    // directions) and the same call encrypts/decrypts.
    bool Ok = mbedtls_aes_setkey_enc(&Ctx, Key.data(), 256) == 0;
    if(Ok) {
        Out.resize(In.size());
        std::array<std::uint8_t, 16> Block = Counter;
        std::array<std::uint8_t, 16> Stream{};
        std::size_t Offset = 0;
        const auto *Src = In.empty() ? nullptr : In.data();
        auto *Dst = Out.empty() ? nullptr : Out.data();
        Ok = In.empty() || mbedtls_aes_crypt_ctr(&Ctx, In.size(), &Offset, Block.data(), Stream.data(), Src, Dst) == 0;
    }
    mbedtls_aes_free(&Ctx);

    if(!Ok) {
        Out.clear();
    }
    return Ok;
}

} // namespace NxMatrix::Crypto