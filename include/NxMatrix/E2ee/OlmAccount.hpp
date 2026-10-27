#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/E2ee/DeviceIdentity.hpp"
#include "NxMatrix/Types.hpp"

#include <cstddef>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace NxMatrix::E2ee {

// Wraps an olm_account: the device identity (curve25519 + ed25519), its
// one-time keys and JSON signing. Pickles are stored base64 in the JSON
// state exported through ExportState/ImportState.
class OlmAccount {
public:
    OlmAccount();
    ~OlmAccount();

    OlmAccount(const OlmAccount &) = delete;
    OlmAccount &operator=(const OlmAccount &) = delete;

    bool CreateNew(const std::string &UserId, const std::string &DeviceId);
    const DeviceIdentity &GetIdentity() const;
    bool IsValid() const;

    nlohmann::json ExportState() const;
    // Returns false on bad pickle key or malformed state.
    bool ImportState(const nlohmann::json &State, const Crypto::KeyBytes &PickleKey);

    // "device_keys" body for POST /keys/upload (self-signed with ed25519).
    nlohmann::json BuildDeviceKeys() const;

    // Fresh one-time keys, each self-signed:
    // {"signed_curve25519:<key id>": {"key": "...", "signatures": {...}}}
    std::map<std::string, nlohmann::json> GenerateSignedOneTimeKeys(std::size_t Count);
    std::size_t GetOneTimeKeyCount() const;

    // ed25519 signature (base64) over the canonical JSON of Object.
    std::string Sign(const nlohmann::json &Object) const;

private:
    void *m_Handle = nullptr;
    ByteArray m_Buffer;
    DeviceIdentity m_Identity;
    Crypto::KeyBytes m_PickleKey{};
    bool m_Valid = false;
    // libolm has no published-key counter; E2eeManager keeps this in sync with
    // the server's one_time_key_counts from /keys/upload responses.
    std::size_t m_PublishedKeyCount = 0;

    friend class OlmSessionManager; // reads the raw olm handle when creating/decrypting sessions
    friend class E2eeManager;       // wires the pickle key and upload bookkeeping
};

} // namespace NxMatrix::E2ee