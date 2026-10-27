#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/E2ee/DeviceIdentity.hpp"
#include "NxMatrix/Types.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace NxMatrix::E2ee {

class OlmAccount;

// Olm (pairwise) sessions used for to-device traffic: room key distribution,
// forwarded keys and key requests.
class OlmSessionManager {
public:
    nlohmann::json ExportState() const;
    bool ImportState(const nlohmann::json &State, const Crypto::KeyBytes &PickleKey);

    bool HasSessionFor(const std::string &TargetCurveKey) const;

    // Starts a session from a claimed one-time key ("signed_curve25519:<id>").
    bool CreateOutboundSession(const DeviceIdentity &TargetDevice, const std::string &OneTimeKeyId, const std::string &SignedOneTimeKey);

    // Encrypts PlainJson to TargetDevice, producing the content of an
    // "m.olm.v1.curve25519-aes-sha2" to-device event.
    bool EncryptToDevice(const DeviceIdentity &TargetDevice, const nlohmann::json &PlainJson, nlohmann::json &OutContent);

    // Decrypts an incoming olm to-device payload (prekey or normal message).
    // OutPlain is the inner JSON ("m.room_key", "m.forwarded_room_key"...).
    bool DecryptFromDevice(const std::string &SenderCurveKey, const nlohmann::json &EncryptedContent, nlohmann::json &OutPlain);

private:
    struct SessionEntry {
        void *Handle = nullptr;
        ByteArray Buffer;
    };

    std::map<std::string, SessionEntry> m_Sessions; // key: device curve25519 key

    OlmAccount *m_Account = nullptr; // our account: inbound sessions are derived from it
    DeviceIdentity m_OurIdentity;    // sender_key/device_id of outgoing olm payloads
    Crypto::KeyBytes m_PickleKey{};  // session pickle key (master key)

    friend class E2eeManager;
    friend class DeviceTracker; // re-binds the account before /keys/claim
};

} // namespace NxMatrix::E2ee