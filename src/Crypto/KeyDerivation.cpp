#include "NxMatrix/Crypto/KeyDerivation.hpp"

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace NxMatrix::Crypto {

KeyBytes KeyDerivation::DeriveKey(const std::string &Secret, const SaltBytes &Salt, std::uint32_t Iterations) {
    KeyBytes Out{};

    mbedtls_md_context_t Ctx;
    mbedtls_md_init(&Ctx);
    const mbedtls_md_info_t *Info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool Ok = Info != nullptr && mbedtls_md_setup(&Ctx, Info, 1) == 0;
    if(Ok) {
        Ok = mbedtls_pkcs5_pbkdf2_hmac(&Ctx, reinterpret_cast<const unsigned char *>(Secret.data()), Secret.size(), Salt.data(),
                                       Salt.size(), Iterations, static_cast<std::uint32_t>(Out.size()), Out.data()) == 0;
    }
    mbedtls_md_free(&Ctx);

    if(!Ok) {
        // Allocation failure inside mbedtls: never hand back a partially
        // derived key (a wrong-but-deterministic key would seal data forever).
        Out.fill(0);
    }
    return Out;
}

} // namespace NxMatrix::Crypto