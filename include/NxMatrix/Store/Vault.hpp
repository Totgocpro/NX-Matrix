#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/Types.hpp"

#include <cstdint>
#include <string>

namespace NxMatrix::Store {

// Seals blobs with a master key derived from the user's PIN or password.
// Sealed layout (little-endian):
//   "NXMV" | version u16 | reserved u16 | KDF iterations u32 | salt 16 | nonce 12 | tag 16 | ciphertext
// Every blob is self-describing (KDF parameters included), so any sealed file
// can be re-opened after a restart with only the user secret.
class Vault {
public:
    // Fresh random salt. Iterations = Crypto::KeyDerivation::DefaultIterations.
    static void Create(const std::string &Secret, Vault &Out);
    // Re-derives the master key from KDF parameters previously sealed in a blob.
    static bool Restore(const std::string &Secret, const Crypto::SaltBytes &Salt, std::uint32_t Iterations, Vault &Out);

    ByteArray Seal(const ByteArray &Plain) const;
    bool Open(const ByteArray &Sealed, ByteArray &OutPlain) const;

    bool IsOpen() const;
    const Crypto::SaltBytes &GetSalt() const;
    std::uint32_t GetIterations() const;
    // Master key in memory (already derived); reused as pickle key for the
    // E2EE state sealed inside AccountData.
    const Crypto::KeyBytes &GetMasterKey() const;

private:
    Crypto::KeyBytes m_MasterKey{};
    Crypto::SaltBytes m_Salt{};
    std::uint32_t m_Iterations = 0;
    bool m_Open = false;
};

} // namespace NxMatrix::Store