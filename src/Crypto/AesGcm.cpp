#include "NxMatrix/Crypto/AesGcm.hpp"

#include "NxMatrix/Crypto/Random.hpp"

#include <mbedtls/gcm.h>

namespace NxMatrix::Crypto {

namespace {

// mbedtls tolerates zero-length buffers but not null pointers everywhere;
// give empty inputs/outputs a stable address.
const std::uint8_t *SafePtr(const std::uint8_t *Ptr, std::size_t Size) {
    static const std::uint8_t kEmpty = 0;
    return (Ptr != nullptr && Size != 0) ? Ptr : &kEmpty;
}

std::uint8_t *MutablePtr(std::uint8_t *Ptr, std::size_t Size) {
    static std::uint8_t kEmpty = 0;
    return (Ptr != nullptr && Size != 0) ? Ptr : &kEmpty;
}

} // namespace

bool AesGcm::Encrypt(const KeyBytes &Key, const ByteArray &Plain, const ByteArray &AdditionalData, SealedData &Out) {
    mbedtls_gcm_context Ctx;
    mbedtls_gcm_init(&Ctx);

    bool Ok = mbedtls_gcm_setkey(&Ctx, MBEDTLS_CIPHER_ID_AES, Key.data(), 256) == 0;
    if(Ok) {
        FillRandom(Out.Nonce.data(), Out.Nonce.size());
        Out.Ciphertext.assign(Plain.size(), 0);
        Ok = mbedtls_gcm_crypt_and_tag(&Ctx, MBEDTLS_GCM_ENCRYPT, Plain.size(), Out.Nonce.data(), Out.Nonce.size(),
                                       SafePtr(AdditionalData.data(), AdditionalData.size()), AdditionalData.size(),
                                       SafePtr(Plain.data(), Plain.size()), MutablePtr(Out.Ciphertext.data(), Out.Ciphertext.size()),
                                       Out.Tag.size(), Out.Tag.data()) == 0;
    }
    mbedtls_gcm_free(&Ctx);

    if(!Ok) {
        Out.Ciphertext.clear();
        Out.Tag = {};
    }
    return Ok;
}

bool AesGcm::Decrypt(const KeyBytes &Key, const SealedData &In, const ByteArray &AdditionalData, ByteArray &OutPlain) {
    mbedtls_gcm_context Ctx;
    mbedtls_gcm_init(&Ctx);

    bool Ok = mbedtls_gcm_setkey(&Ctx, MBEDTLS_CIPHER_ID_AES, Key.data(), 256) == 0;
    if(Ok) {
        // Decrypt into a scratch buffer: on auth failure mbedtls zeroizes it,
        // so OutPlain never receives partial or unauthenticated plaintext.
        ByteArray Temp(In.Ciphertext.size(), 0);
        Ok = mbedtls_gcm_auth_decrypt(&Ctx, In.Ciphertext.size(), In.Nonce.data(), In.Nonce.size(),
                                      SafePtr(AdditionalData.data(), AdditionalData.size()), AdditionalData.size(), In.Tag.data(),
                                      In.Tag.size(), SafePtr(In.Ciphertext.data(), In.Ciphertext.size()),
                                      MutablePtr(Temp.data(), Temp.size())) == 0;
        if(Ok) {
            OutPlain = std::move(Temp);
        }
    }
    mbedtls_gcm_free(&Ctx);
    return Ok;
}

} // namespace NxMatrix::Crypto