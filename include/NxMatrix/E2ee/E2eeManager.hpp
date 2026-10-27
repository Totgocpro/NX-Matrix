#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/E2ee/CrossSigning.hpp"
#include "NxMatrix/E2ee/DeviceIdentity.hpp"
#include "NxMatrix/E2ee/KeyBackup.hpp"
#include "NxMatrix/E2ee/Verification.hpp"
#include "NxMatrix/Matrix/Models.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace NxMatrix::Matrix {
class Client;
}

namespace NxMatrix::E2ee {

class OlmAccount;
class OlmSessionManager;
class MegolmManager;
class DeviceTracker;

// End-to-end encryption for room events (Olm + Megolm, backed by libolm).
//
// Covered: device identity and /keys upload|query|claim, olm to-device
// sessions, megolm room sessions (sharing + rotation), decryption of
// m.room.encrypted events, best-effort missing-key requests.
//
// It also OWNS the sub-managers for SAS verification (m.sas.v1),
// cross-signing and the server-side key backup, exposed through getters.
class E2eeManager {
public:
    explicit E2eeManager(Matrix::Client &Api);
    ~E2eeManager();

    void SetIdentity(const std::string &UserId, const std::string &DeviceId);
    void SetMasterKey(const Crypto::KeyBytes &MasterKey);
    bool IsReady() const;

    // Persistent state lives in AccountData::OlmState (all pickles, base64).
    nlohmann::json ExportState() const;
    void ImportState(const nlohmann::json &State);

    // Blocking: uploads device keys and replenishes one-time keys when the
    // server reports fewer than MinimumOneTimeKeys left. Returns false when
    // the device has no usable identity yet or the upload was rejected.
    bool EnsureKeysUploaded(std::size_t MinimumOneTimeKeys = 10);

    // Blocking: encrypts an event for an encrypted room, creating/sharing the
    // megolm session as needed. MemberUserIds = users currently joined/invited.
    bool EncryptRoomEvent(const std::string &RoomId, const std::vector<std::string> &MemberUserIds, const std::string &EventType,
                          const nlohmann::json &PlainContent, nlohmann::json &OutEncryptedContent, std::string *OutError = nullptr);

    // Receive path (called from Session): true = OutPlainContent/OutEventType set,
    // false with OutFailed = permanently undecryptable (e.g. missing session).
    bool DecryptRoomEvent(const Matrix::Event &EncryptedEvent, nlohmann::json &OutPlainContent, std::string &OutEventType, bool &OutFailed);

    // Sync hooks: to-device events, device list changes and the server's
    // one-time key count ("signed_curve25519").
    void OnSync(const std::vector<nlohmann::json> &ToDeviceEvents, const std::vector<std::string> &ChangedDeviceLists,
                std::size_t OneTimeKeyCount);

    // Membership changed in a room: rotate the megolm session and share the
    // new key with all current member devices.
    void OnRoomMembershipChanged(const std::string &RoomId, const std::vector<std::string> &MemberUserIds);

    // Best-effort "m.room_key.request" to our own devices (and the sender).
    void RequestMissingKey(const std::string &RoomId, const std::string &SenderKey, const std::string &SessionId,
                           const std::string &RequestId);

    // For the settings screen (device id + key fingerprints).
    DeviceIdentity GetDeviceIdentity() const;

    // Owned sub-managers, driven directly by the UI (blocking: JobQueue).
    VerificationManager &GetVerification();
    CrossSigningManager &GetCrossSigning();
    KeyBackup &GetKeyBackup();
    DeviceTracker &GetDevices();

private:
    Matrix::Client &m_Api;
    Crypto::KeyBytes m_MasterKey{};
    std::string m_UserId;
    std::string m_DeviceId;

    OlmAccount *m_Account = nullptr;
    OlmSessionManager *m_Sessions = nullptr;
    MegolmManager *m_Megolm = nullptr;
    DeviceTracker *m_Devices = nullptr;
    VerificationManager *m_Verification = nullptr;
    CrossSigningManager *m_CrossSigning = nullptr;
    KeyBackup *m_Backup = nullptr;
    std::size_t m_LastOneTimeKeyCount = 0;
    bool m_KeysUploaded = false;
    bool m_MasterKeySet = false;

    // Pushes the master key into every component that pickles its own state.
    void ApplyPickleKey();
    // Pushes the account identity into the session/megolm senders (after create/import).
    void ApplyIdentity();
    // Olm payload envelope required by the spec for m.olm.v1 plaintext.
    nlohmann::json BuildOlmPayload(const std::string &Type, const nlohmann::json &Content, const DeviceIdentity &Target) const;
    // Olm-encrypts the room key to every target device (skip: already delivered) and sends it to-device.
    void ShareRoomKey(const std::string &RoomId, const std::vector<DeviceIdentity> &Targets);
    // Serves an m.room_key.request from our own outbound session when we still own it.
    void HandleKeyRequest(const std::string &SenderUserId, const nlohmann::json &RequestContent);

    std::map<std::string, std::string> m_SharedSessionId;            // room id -> megolm session id we shared
    std::map<std::string, std::vector<std::string>> m_SharedDevices; // room id -> "user|device" targets that got it
    std::map<std::string, std::int64_t> m_KeyRequestTime;            // session id -> last m.room_key.request (ms)
};

} // namespace NxMatrix::E2ee