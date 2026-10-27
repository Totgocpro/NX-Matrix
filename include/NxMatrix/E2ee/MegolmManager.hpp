#pragma once
#include "NxMatrix/Crypto/Types.hpp"
#include "NxMatrix/E2ee/DeviceIdentity.hpp"
#include "NxMatrix/Types.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace NxMatrix::E2ee {

// Megolm (group) sessions: one outbound session per room for sending, one
// inbound session per (sender key, session id) for receiving.
class MegolmManager {
public:
    nlohmann::json ExportState() const;
    bool ImportState(const nlohmann::json &State, const Crypto::KeyBytes &PickleKey);

    // --- Sending ---
    bool EnsureOutboundSession(const std::string &RoomId);
    // Rotates when the session hit MaxMessages messages or MaxAgeMs age.
    bool RotateIfNeeded(const std::string &RoomId, std::int64_t NowMs);
    void DropOutboundSession(const std::string &RoomId);
    bool EncryptRoomEvent(const std::string &RoomId, const std::string &EventType, const nlohmann::json &PlainContent,
                          nlohmann::json &OutEncryptedContent);
    // "m.room_key" content describing the current outbound session.
    nlohmann::json BuildRoomKeyMessage(const std::string &RoomId) const;

    // --- Receiving ---
    // Accepts "m.room_key" / "m.forwarded_room_key" content (Forwarded = no
    // chain index trust, still decrypts).
    bool ImportRoomKey(const nlohmann::json &RoomKeyContent, const std::string &SenderCurveKey, bool Forwarded);
    bool CanDecrypt(const std::string &SenderCurveKey, const std::string &SessionId) const;
    bool DecryptRoomEvent(const std::string &SenderCurveKey, const std::string &SessionId, const std::string &Ciphertext,
                          nlohmann::json &OutPlainContent, std::string &OutEventType);

    // --- Backup export (server-side key backup) ---
    struct ExportedSession {
        std::string RoomId;
        std::string SessionId;
        std::string SessionKey;     // v1 session-export format (base64), olm_export_inbound_group_session
        std::string SenderCurveKey; // inbound index owner = sender device curve key ("" when unknown)
        int FirstMessageIndex = 0;
        int ForwardedCount = 0;
    };
    // Every decryptable inbound session, exportable as-is for /room_keys upload.
    // Sessions whose room id is unknown or whose export fails are skipped.
    std::vector<ExportedSession> ExportAllSessions() const;
    std::size_t GetInboundSessionCount() const;

    static constexpr std::size_t MaxMessages = 100;
    static constexpr std::int64_t MaxAgeMs = 7LL * 24 * 60 * 60 * 1000;

private:
    struct OutboundEntry {
        void *Handle = nullptr;
        ByteArray Buffer;
        std::int64_t CreatedMs = 0;
        std::size_t MessageCount = 0;
    };

    struct InboundEntry {
        void *Handle = nullptr;
        ByteArray Buffer;
    };

    std::map<std::string, OutboundEntry> m_Outbound;    // key: room id
    std::map<std::string, InboundEntry> m_Inbound;      // key: "<sender curve key>|<session id>"
    std::map<std::string, std::string> m_InboundRoomId; // session id -> room id (persisted, needed for backups)

    DeviceIdentity m_OurIdentity;   // sender_key/device_id of encrypted room events
    Crypto::KeyBytes m_PickleKey{}; // session pickle key (master key)

    friend class E2eeManager;
};

} // namespace NxMatrix::E2ee