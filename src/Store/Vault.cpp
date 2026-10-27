#include "NxMatrix/Store/Vault.hpp"

#include "NxMatrix/Crypto/KeyDerivation.hpp"
#include "NxMatrix/Crypto/Random.hpp"

#include <mbedtls/gcm.h>

#include <algorithm>
#include <cstring>

namespace NxMatrix::Store {

namespace {

// "NXMV" | version u16 | reserved u16 | iterations u32 | salt 16 | nonce 12
// = everything preceding the tag; it is authenticated as AAD, so KDF
// parameters and nonce can never drift from the ciphertext.
constexpr std::size_t kHeaderSize = 40;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kSealedOverhead = kHeaderSize + kTagSize;
constexpr std::uint16_t kVersion = 1;

const std::uint8_t *SafePtr(const std::uint8_t *Ptr, std::size_t Size) {
    static const std::uint8_t kEmpty = 0;
    return (Ptr != nullptr && Size != 0) ? Ptr : &kEmpty;
}

std::uint8_t *MutablePtr(std::uint8_t *Ptr, std::size_t Size) {
    static std::uint8_t kEmpty = 0;
    return (Ptr != nullptr && Size != 0) ? Ptr : &kEmpty;
}

void WriteU16Le(ByteArray &Buf, std::size_t Offset, std::uint16_t Value) {
    Buf[Offset] = static_cast<std::uint8_t>(Value & 0xFF);
    Buf[Offset + 1] = static_cast<std::uint8_t>((Value >> 8) & 0xFF);
}

void WriteU32Le(ByteArray &Buf, std::size_t Offset, std::uint32_t Value) {
    for(std::size_t I = 0; I < 4; ++I) {
        Buf[Offset + I] = static_cast<std::uint8_t>((Value >> (8 * I)) & 0xFF);
    }
}

std::uint16_t ReadU16Le(const ByteArray &Buf, std::size_t Offset) {
    return static_cast<std::uint16_t>(Buf[Offset] | (Buf[Offset + 1] << 8));
}

} // namespace

void Vault::Create(const std::string &Secret, Vault &Out) {
    Crypto::FillRandom(Out.m_Salt.data(), Out.m_Salt.size());
    Out.m_Iterations = Crypto::KeyDerivation::DefaultIterations;
    Out.m_MasterKey = Crypto::KeyDerivation::DeriveKey(Secret, Out.m_Salt, Out.m_Iterations);
    Out.m_Open = true;
}

bool Vault::Restore(const std::string &Secret, const Crypto::SaltBytes &Salt, std::uint32_t Iterations, Vault &Out) {
    if(Iterations == 0) {
        return false; // corrupt KDF parameters cannot re-derive anything usable
    }
    Out.m_Salt = Salt;
    Out.m_Iterations = Iterations;
    Out.m_MasterKey = Crypto::KeyDerivation::DeriveKey(Secret, Out.m_Salt, Out.m_Iterations);
    Out.m_Open = true;
    return true;
}

ByteArray Vault::Seal(const ByteArray &Plain) const {
    if(!m_Open) {
        return {};
    }

    ByteArray Header(kHeaderSize, 0);
    Header[0] = 'N';
    Header[1] = 'X';
    Header[2] = 'M';
    Header[3] = 'V';
    WriteU16Le(Header, 4, kVersion);
    WriteU16Le(Header, 6, 0); // reserved
    WriteU32Le(Header, 8, m_Iterations);
    std::copy(m_Salt.begin(), m_Salt.end(), Header.begin() + 12);
    // The nonce lives inside the header/AAD, so it must be drawn before the
    // GCM call (Vault owns the full container layout, not Crypto::SealedData).
    Crypto::FillRandom(Header.data() + 28, 12);

    ByteArray Ciphertext(Plain.size(), 0);
    Crypto::TagBytes Tag{};

    mbedtls_gcm_context Ctx;
    mbedtls_gcm_init(&Ctx);
    bool Ok = mbedtls_gcm_setkey(&Ctx, MBEDTLS_CIPHER_ID_AES, m_MasterKey.data(), 256) == 0;
    if(Ok) {
        Ok = mbedtls_gcm_crypt_and_tag(&Ctx, MBEDTLS_GCM_ENCRYPT, Plain.size(), Header.data() + 28, 12, Header.data(), Header.size(),
                                       SafePtr(Plain.data(), Plain.size()), MutablePtr(Ciphertext.data(), Ciphertext.size()), Tag.size(),
                                       Tag.data()) == 0;
    }
    mbedtls_gcm_free(&Ctx);
    if(!Ok) {
        return {};
    }

    ByteArray Sealed;
    Sealed.reserve(kSealedOverhead + Ciphertext.size());
    Sealed.insert(Sealed.end(), Header.begin(), Header.end());
    Sealed.insert(Sealed.end(), Tag.begin(), Tag.end());
    Sealed.insert(Sealed.end(), Ciphertext.begin(), Ciphertext.end());
    return Sealed;
}

bool Vault::Open(const ByteArray &Sealed, ByteArray &OutPlain) const {
    if(!m_Open) {
        return false;
    }
    if(Sealed.size() < kSealedOverhead) {
        return false;
    }
    if(std::memcmp(Sealed.data(), "NXMV", 4) != 0) {
        return false;
    }
    if(ReadU16Le(Sealed, 4) != kVersion) {
        return false;
    }

    Crypto::NonceBytes Nonce{};
    std::copy_n(Sealed.begin() + 28, Nonce.size(), Nonce.begin());
    Crypto::TagBytes Tag{};
    std::copy_n(Sealed.begin() + kHeaderSize, Tag.size(), Tag.begin());
    const ByteArray Ciphertext(Sealed.begin() + kSealedOverhead, Sealed.end());

    // Decrypt to a scratch buffer: OutPlain is only touched on full GCM auth.
    ByteArray Temp(Ciphertext.size(), 0);
    mbedtls_gcm_context Ctx;
    mbedtls_gcm_init(&Ctx);
    bool Ok = mbedtls_gcm_setkey(&Ctx, MBEDTLS_CIPHER_ID_AES, m_MasterKey.data(), 256) == 0;
    if(Ok) {
        Ok = mbedtls_gcm_auth_decrypt(&Ctx, Ciphertext.size(), Nonce.data(), Nonce.size(), Sealed.data(), kHeaderSize, Tag.data(),
                                      Tag.size(), SafePtr(Ciphertext.data(), Ciphertext.size()), MutablePtr(Temp.data(), Temp.size())) == 0;
    }
    mbedtls_gcm_free(&Ctx);

    if(!Ok) {
        return false; // wrong secret or tampered container
    }
    OutPlain = std::move(Temp);
    return true;
}

bool Vault::IsOpen() const {
    return m_Open;
}

const Crypto::SaltBytes &Vault::GetSalt() const {
    return m_Salt;
}

std::uint32_t Vault::GetIterations() const {
    return m_Iterations;
}

const Crypto::KeyBytes &Vault::GetMasterKey() const {
    return m_MasterKey;
}

} // namespace NxMatrix::Store