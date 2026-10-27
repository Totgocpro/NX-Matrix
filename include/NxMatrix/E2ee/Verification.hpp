#pragma once
#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace NxMatrix::Matrix {
class Client;
}

namespace NxMatrix::E2ee {

class OlmAccount;
class OlmSessionManager;
class DeviceTracker;

// Progress of a short-auth-string (m.sas.v1) verification with another device
// of ours: emoji/decimal comparison on both screens, then MAC exchange.
enum class VerificationStep {
    Idle,
    RequestSent,     // we asked the other device
    RequestReceived, // they asked us, waiting for our Accept
    Started,         // m.sas.v1 agreed, keys being exchanged
    EmojisReady,     // emoji/decimal sequence available: compare with the other screen
    AcceptedByUs,    // we confirmed the emojis match, MACs in flight
    Verified,        // MACs checked: the device is trusted and signed
    Cancelled,
    Failed,
};

struct VerificationSnapshot {
    VerificationStep Step = VerificationStep::Idle;
    std::string OtherUserId;
    std::string OtherDeviceId;
    std::string TransactionId;
    std::vector<int> EmojiIndexes;  // 8 indexes (0..63) into the spec emoji table
    std::vector<int> DecimalValues; // 6 values 0..999 (same secret, decimal form)
    std::string Error;
};

// Drives one m.sas.v1 verification at a time. Network calls are blocking
// (run them on Core::JobQueue); the UI polls GetSnapshot() on the render
// thread. Verified devices are persisted through ExportState (sealed inside
// AccountData::OlmState).
class VerificationManager {
public:
    VerificationManager(Matrix::Client &Api, OlmAccount &Account, OlmSessionManager &Sessions, DeviceTracker &Devices);

    bool IsBusy() const;
    VerificationSnapshot GetSnapshot() const;

    // Start SAS with one of our own other devices.
    bool Start(const std::string &UserId, const std::string &DeviceId);

    // Incoming request (Step == RequestReceived): accept or decline it.
    bool AcceptIncoming();
    bool RejectIncoming();

    // Emoji phase: both screens show the same sequence -> confirm (exchanges
    // MACs and marks the device verified + signed), or reject.
    bool ConfirmEmojisMatch();
    bool RejectEmojis();

    // Aborts any in-progress verification (sends m.key.verification.cancel).
    void Cancel();

    // Routes one to-device m.key.verification.* event (called from
    // E2eeManager::OnSync for both plain and olm-wrapped payloads).
    void HandleToDevice(const nlohmann::json &Event);

    bool IsVerified(const std::string &UserId, const std::string &DeviceId) const;
    nlohmann::json ExportState() const;
    void ImportState(const nlohmann::json &State);

private:
    Matrix::Client &m_Api;
    OlmAccount &m_Account;
    OlmSessionManager &m_Sessions;
    DeviceTracker &m_Devices;

    mutable std::mutex m_Mutex;
    VerificationSnapshot m_Snapshot;
    std::vector<std::string> m_VerifiedDevices; // "<user>|<device>"

    // Private state of the single in-flight m.sas.v1 run (guarded by m_Mutex).
    void *m_Sas = nullptr;                   // olm_sas_t placed over m_SasBuffer, recreated per transaction
    std::vector<std::uint8_t> m_SasBuffer;   // backing memory for m_Sas
    bool m_WeSentStart = false;              // we took the m.key.verification.start role instead of accepting theirs
    bool m_Confirmed = false;                // our user confirmed the emojis and our MAC went out
    std::string m_OurSasPub;                 // our ephemeral key, unpadded base64
    std::string m_TheirSasKey;               // their m.key.verification.key payload
    std::string m_TheirCommitment;           // accept.commitment, re-checked when their key arrives
    std::string m_MacMethod;                 // negotiated code: hkdf-hmac-sha256[.v2]
    nlohmann::json m_StartContent{};         // our start content, re-hashed by the commitment check
    std::vector<std::string> m_TheirMethods; // methods the other side offered
    nlohmann::json m_PendingMac{};           // their MAC arrived before our user confirmed

    // The published copy has its own lock: UI readers copy it instantly and
    // never wait behind the blocking HTTP the state machine issues under m_Mutex.
    mutable std::mutex m_SnapshotMutex;

    // Every helper below runs with m_Mutex already held by its caller.
    void ResetTransactionLocked();
    VerificationStep CurrentStepLocked() const;
    void SetStepLocked(VerificationStep Step);
    void FailLocked(const std::string &Error);
    void GetPeerLocked(std::string &OutUserId, std::string &OutDeviceId, std::string &OutTransactionId) const;
    bool SendVerification(const std::string &UserId, const std::string &DeviceId, const std::string &Type, const nlohmann::json &Content);
    void SendCancelLocked(const std::string &Code, const std::string &Reason);
    bool EnsureSasLocked();
    std::string BuildSasInfoLocked() const;
    std::string CalculateMacLocked(const std::string &Input, const std::string &Info);
    bool ComputeSasLocked();
    void TryFinishKeyExchangeLocked();
    void VerifyPendingMacLocked();
    void HandleRequestLocked(const std::string &Sender, const nlohmann::json &Content, const std::string &TransactionId);
    void HandleReadyLocked(const std::string &Sender, const nlohmann::json &Content, const std::string &TransactionId);
    void HandleStartLocked(const std::string &Sender, const nlohmann::json &Content, const std::string &TransactionId);
    void HandleAcceptLocked(const nlohmann::json &Content, const std::string &TransactionId);
    void HandleKeyLocked(const nlohmann::json &Content, const std::string &TransactionId);
    void HandleMacLocked(const nlohmann::json &Content, const std::string &TransactionId);
};

} // namespace NxMatrix::E2ee