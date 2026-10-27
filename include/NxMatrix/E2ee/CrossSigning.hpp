#pragma once
#include <nlohmann/json.hpp>

#include <string>

namespace NxMatrix::Matrix {
class Client;
}

namespace NxMatrix::E2ee {

class OlmAccount;
struct DeviceIdentity;

// Master / self-signing / user-signing keys for cross-signing.
// Private keys live in ExportState (sealed inside AccountData::OlmState);
// public keys are uploaded to /keys/device_signing/upload.
//
// Signing another user's master key (trust) needs OUR user-signing key:
// that is what makes "verified" travel between our sessions.
class CrossSigningManager {
public:
    CrossSigningManager(Matrix::Client &Api, OlmAccount &Account);

    bool IsSetUp() const; // master + self-signing + user-signing keys present locally
    // Short fingerprint of the master public key for the settings screen.
    std::string GetMasterKeyFingerprint() const;

    // Blocking (JobQueue): generates the three key pairs, uploads them
    // (/keys/device_signing/upload, password UIAA when the server asks),
    // self-signs our device and self-signs the master key, then uploads the
    // signatures (/keys/signatures/upload). Password may be empty when the
    // server accepts the call without a UIAA stage.
    bool SetUpKeys(const std::string &Password);

    // Blocking: signs a device (with our self-signing key) or a user's master
    // key (with our user-signing key) and uploads via /keys/signatures/upload.
    bool SignDevice(const DeviceIdentity &Device);
    bool SignUser(const std::string &UserId);

    // Blocking: /keys/query for the user and checks the signatures on their
    // keys against the local trust anchors (best effort verification report).
    bool VerifyRemoteDevice(const std::string &UserId, const std::string &DeviceId);

    nlohmann::json ExportState() const;
    void ImportState(const nlohmann::json &State);

private:
    Matrix::Client &m_Api;
    OlmAccount &m_Account;
    nlohmann::json m_State; // {master, self_signing, user_signing} private material
};

} // namespace NxMatrix::E2ee