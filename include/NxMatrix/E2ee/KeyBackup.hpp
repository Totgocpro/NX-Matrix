#pragma once
#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace NxMatrix::Matrix {
class Client;
}

namespace NxMatrix::E2ee {

class MegolmManager;
class OlmAccount;

// Server-side key backup (4S): encrypts exported megolm session data with a
// curve25519 key (libolm olm/pk.h) and stores it at /room_keys/*.
// The private key (recovery key or passphrase-derived) is sealed inside
// AccountData::OlmState through ExportState.
class KeyBackup {
public:
    struct Status {
        bool Exists = false;       // a backup version exists on the server
        bool KeyAvailable = false; // and we hold its private key locally
        std::string Version;
        std::string Algorithm;      // m.megolm_backup.v1.curve25519-aes-sha2
        std::string KeyFingerprint; // display prefix of the recovery key
        int UploadedSessions = 0;
        int LocalSessions = 0;
    };

    KeyBackup(Matrix::Client &Api, MegolmManager &Megolm);

    nlohmann::json ExportState() const;
    void ImportState(const nlohmann::json &State);
    bool HasKey() const;

    // Blocking: GET /room_keys/version (+ local session counts).
    Status GetStatus();

    // Blocking: creates a backup version. Derives the key from Passphrase
    // (or generates a raw recovery key when Passphrase is empty), uploads the
    // version and stores the private key locally. OutRecoveryKey is shown to
    // the user ONCE (base58 recovery key).
    bool CreateBackup(const std::string &Passphrase, std::string &OutRecoveryKey);

    // Blocking: activates an existing backup by pasting the recovery key
    // (verifies it by fetching version info and matching the public key).
    bool ActivateWithRecoveryKey(const std::string &RecoveryKey);

    // Blocking: PUT all local inbound megolm sessions (only the ones the
    // server does not have yet) / GET the backup into the local store.
    bool UploadAllSessions();
    bool RestoreAllSessions();

    // Records a newly imported inbound session for the next upload push
    // (called by E2eeManager after every m.room_key import).
    void NoteNewSession(const std::string &RoomId, const std::string &SessionId);

    // Cheap, throttled push of the dirty sessions only (no-op without a key,
    // while a push is in flight or inside the internal throttle window).
    // Called from the sync loop; blocking.
    void PushDirtySessions();

private:
    Matrix::Client &m_Api;
    MegolmManager &m_Megolm;

    mutable std::mutex m_Mutex;
    nlohmann::json m_State;                   // {version, algorithm, private_key, salt, iterations, recovery_key, ...}
    std::vector<std::string> m_DirtySessions; // "<room>|<session>" awaiting upload

    // Wired by E2eeManager right after construction (same pattern as
    // OlmSessionManager::m_Account): signs auth_data with our ed25519 key.
    OlmAccount *m_Account = nullptr;
    // Throttle + single-flight guard for PushDirtySessions (sync-loop driven).
    std::chrono::steady_clock::time_point m_LastPush{};
    bool m_Pushing = false;

    friend class E2eeManager;
};

} // namespace NxMatrix::E2ee