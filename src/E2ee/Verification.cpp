// m.sas.v1 short-authentication-string verification between two of our own
// devices: request/ready handshake, ephemeral key exchange with a hash
// commitment, emoji/decimal SAS derived from the shared secret, then the MAC
// round that marks the other device verified (and signed, when cross-signing
// is set up). Wire shapes follow the Matrix Client-Server API v1.19
// ("Key verification framework" and "Short Authentication String (SAS)
// verification"): request -> ready -> start -> accept -> key -> key ->
// mac -> mac -> done.
//
// The whole run is one transaction guarded by m_Mutex; every phase change is
// published to m_Snapshot, which the UI polls. Blocking HTTP therefore never
// runs under m_SnapshotMutex, so the render thread's GetSnapshot() stays
// instant even while a send is in flight.
#include "NxMatrix/E2ee/Verification.hpp"

#include "NxMatrix/Crypto/Hash.hpp"
#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/E2ee/DeviceTracker.hpp"
#include "NxMatrix/E2ee/OlmAccount.hpp"
#include "NxMatrix/E2ee/OlmSessionManager.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Util/Base64.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/String.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <olm/sas.h>

#include <algorithm>

namespace NxMatrix::E2ee {

namespace {

// The only verification method this client implements (advertised in
// request/ready, carried in start, selected in accept).
constexpr const char *SasMethod = "m.sas.v1";
// The only key agreement implemented: the spec discourages new code from
// using the deprecated plain "curve25519" variant.
constexpr const char *KeyAgreement = "curve25519-hkdf-sha256";
constexpr const char *MacMethodV2 = "hkdf-hmac-sha256.v2";
constexpr const char *MacMethodV1 = "hkdf-hmac-sha256";

bool IsActiveStep(VerificationStep Step) {
    switch(Step) {
    case VerificationStep::RequestSent:
    case VerificationStep::RequestReceived:
    case VerificationStep::Started:
    case VerificationStep::EmojisReady:
    case VerificationStep::AcceptedByUs:
        return true;
    default:
        return false;
    }
}

// Random hex id for the HTTP /sendToDevice transaction; unrelated to the
// verification transaction_id carried inside the event content.
std::string MakeHttpTransactionId() {
    static const char *Hex = "0123456789abcdef";
    const ByteArray Bytes = Crypto::RandomBytes(16);
    std::string Out;
    Out.reserve(32);
    for(const std::uint8_t Byte : Bytes) {
        Out.push_back(Hex[Byte >> 4]);
        Out.push_back(Hex[Byte & 0x0F]);
    }
    return Out;
}

// Spec fields (commitment, MACs, SAS keys) are unpadded base64; the shared
// helper emits padding, so strip it here.
std::string UnpaddedBase64(const ByteArray &Data) {
    std::string Out = Util::Base64Encode(Data);
    while(!Out.empty() && Out.back() == '=') {
        Out.pop_back();
    }
    return Out;
}

// A MAC we computed and a MAC they sent stand for the same secret: compare
// them without an early exit on mismatch.
bool ConstantTimeEqual(const std::string &A, const std::string &B) {
    if(A.size() != B.size()) {
        return false;
    }
    std::uint8_t Diff = 0;
    for(std::size_t Index = 0; Index < A.size(); ++Index) {
        Diff |= static_cast<std::uint8_t>(A[Index]) ^ static_cast<std::uint8_t>(B[Index]);
    }
    return Diff == 0;
}

bool Contains(const std::vector<std::string> &List, const std::string &Value) {
    return std::find(List.begin(), List.end(), Value) != List.end();
}

// Tolerant list reader: peers may omit or mistype fields, and const
// operator[] on a missing key is undefined behaviour in nlohmann.
std::vector<std::string> GetStringList(const nlohmann::json &Object, const std::string &Key) {
    std::vector<std::string> Out;
    if(!Object.is_object()) {
        return Out;
    }
    const auto It = Object.find(Key);
    if(It == Object.end() || !It->is_array()) {
        return Out;
    }
    for(const nlohmann::json &Item : *It) {
        if(Item.is_string()) {
            Out.push_back(Item.get<std::string>());
        }
    }
    return Out;
}

} // namespace

VerificationManager::VerificationManager(Matrix::Client &Api, OlmAccount &Account, OlmSessionManager &Sessions, DeviceTracker &Devices)
    : m_Api(Api), m_Account(Account), m_Sessions(Sessions), m_Devices(Devices) {}

bool VerificationManager::IsBusy() const {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    return IsActiveStep(m_Snapshot.Step);
}

VerificationSnapshot VerificationManager::GetSnapshot() const {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    return m_Snapshot;
}

// --- state helpers (m_Mutex held) ---

void VerificationManager::ResetTransactionLocked() {
    m_Sas = nullptr;
    m_SasBuffer.clear();
    m_WeSentStart = false;
    m_Confirmed = false;
    m_OurSasPub.clear();
    m_TheirSasKey.clear();
    m_TheirCommitment.clear();
    m_MacMethod.clear();
    m_StartContent = nlohmann::json::object();
    m_TheirMethods.clear();
    m_PendingMac = nlohmann::json::object();
}

VerificationStep VerificationManager::CurrentStepLocked() const {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    return m_Snapshot.Step;
}

void VerificationManager::SetStepLocked(VerificationStep Step) {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    m_Snapshot.Step = Step;
}

void VerificationManager::FailLocked(const std::string &Error) {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    m_Snapshot.Step = VerificationStep::Failed;
    m_Snapshot.Error = Error;
}

void VerificationManager::GetPeerLocked(std::string &OutUserId, std::string &OutDeviceId, std::string &OutTransactionId) const {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    OutUserId = m_Snapshot.OtherUserId;
    OutDeviceId = m_Snapshot.OtherDeviceId;
    OutTransactionId = m_Snapshot.TransactionId;
}

bool VerificationManager::SendVerification(const std::string &UserId, const std::string &DeviceId, const std::string &Type,
                                           const nlohmann::json &Content) {
    // Olm-wrap only when a session with the target is already open;
    // verification messages are equally valid as plain to-device events
    // (E2eeManager decrypts wrapped variants before routing them to us).
    const std::vector<DeviceIdentity> Devices = m_Devices.GetDevices(UserId);
    const DeviceIdentity *Target = nullptr;
    for(const DeviceIdentity &Device : Devices) {
        if(Device.DeviceId == DeviceId) {
            Target = &Device;
            break;
        }
    }
    if(Target != nullptr && !Target->Curve25519Key.empty() && m_Sessions.HasSessionFor(Target->Curve25519Key)) {
        const DeviceIdentity &Ours = m_Account.GetIdentity();
        if(!Ours.Ed25519Key.empty()) {
            nlohmann::json Payload;
            Payload["type"] = Type;
            Payload["content"] = Content;
            Payload["sender"] = m_Api.GetUserId();
            Payload["recipient"] = UserId;
            Payload["keys"] = nlohmann::json{{"ed25519", Ours.Ed25519Key}};
            Payload["recipient_keys"] = nlohmann::json{{"ed25519", Target->Ed25519Key}};
            nlohmann::json Encrypted;
            if(m_Sessions.EncryptToDevice(*Target, Payload, Encrypted)) {
                nlohmann::json Messages;
                Messages[UserId][DeviceId] = Encrypted;
                return m_Api.SendToDevice("m.room.encrypted", Messages, MakeHttpTransactionId()).Ok;
            }
        }
    }
    nlohmann::json Messages;
    Messages[UserId][DeviceId] = Content;
    return m_Api.SendToDevice(Type, Messages, MakeHttpTransactionId()).Ok;
}

void VerificationManager::SendCancelLocked(const std::string &Code, const std::string &Reason) {
    std::string UserId;
    std::string DeviceId;
    std::string TransactionId;
    GetPeerLocked(UserId, DeviceId, TransactionId);
    if(UserId.empty() || DeviceId.empty() || TransactionId.empty()) {
        return;
    }
    nlohmann::json Content;
    Content["transaction_id"] = TransactionId;
    Content["code"] = Code;
    Content["reason"] = Reason;
    (void)SendVerification(UserId, DeviceId, "m.key.verification.cancel", Content); // best effort
}

// --- SAS key material ---

bool VerificationManager::EnsureSasLocked() {
    if(m_Sas != nullptr) {
        return true;
    }
    m_SasBuffer.assign(olm_sas_size(), 0);
    auto *Sas = olm_sas(m_SasBuffer.data());
    ByteArray Random = Crypto::RandomBytes(olm_create_sas_random_length(Sas));
    if(olm_create_sas(Sas, Random.data(), Random.size()) != 0) {
        m_SasBuffer.clear();
        return false;
    }
    std::vector<std::uint8_t> Pub(olm_sas_pubkey_length(Sas));
    if(olm_sas_get_pubkey(Sas, Pub.data(), Pub.size()) != 0) {
        m_SasBuffer.clear();
        return false;
    }
    m_Sas = Sas;
    m_OurSasPub.assign(reinterpret_cast<const char *>(Pub.data()), Pub.size());
    return true;
}

std::string VerificationManager::BuildSasInfoLocked() const {
    std::string TheirUser;
    std::string TheirDevice;
    std::string TransactionId;
    GetPeerLocked(TheirUser, TheirDevice, TransactionId);
    const std::string OurUser = m_Api.GetUserId();
    const std::string OurDevice = m_Api.GetDeviceId();
    // Spec orders the info by message roles - whoever sent start first, then
    // whoever sent accept - not by "us/them", so both devices derive the same
    // string no matter which side of the handshake we are on.
    const std::string StartUser = m_WeSentStart ? OurUser : TheirUser;
    const std::string StartDevice = m_WeSentStart ? OurDevice : TheirDevice;
    const std::string StartKey = m_WeSentStart ? m_OurSasPub : m_TheirSasKey;
    const std::string AcceptUser = m_WeSentStart ? TheirUser : OurUser;
    const std::string AcceptDevice = m_WeSentStart ? TheirDevice : OurDevice;
    const std::string AcceptKey = m_WeSentStart ? m_TheirSasKey : m_OurSasPub;
    return "MATRIX_KEY_VERIFICATION_SAS|" + StartUser + "|" + StartDevice + "|" + StartKey + "|" + AcceptUser + "|" + AcceptDevice + "|" +
           AcceptKey + "|" + TransactionId;
}

std::string VerificationManager::CalculateMacLocked(const std::string &Input, const std::string &Info) {
    if(m_Sas == nullptr) {
        return {};
    }
    auto *Sas = static_cast<OlmSAS *>(m_Sas);
    std::vector<std::uint8_t> Mac(olm_sas_mac_length(Sas));
    // hkdf-hmac-sha256.v2 is libolm's "fixed base64" variant (correct base64);
    // plain hkdf-hmac-sha256 keeps the original encoder for wire compatibility
    // with older clients, per the notes in olm/sas.h and the spec.
    const std::size_t Ret =
        (m_MacMethod == MacMethodV2)
            ? olm_sas_calculate_mac_fixed_base64(Sas, Input.data(), Input.size(), Info.data(), Info.size(), Mac.data(), Mac.size())
            : olm_sas_calculate_mac(Sas, Input.data(), Input.size(), Info.data(), Info.size(), Mac.data(), Mac.size());
    if(Ret != 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(Mac.data()), Mac.size());
}

bool VerificationManager::ComputeSasLocked() {
    if(m_Sas == nullptr) {
        FailLocked("Verification state was lost");
        return false;
    }
    auto *Sas = static_cast<OlmSAS *>(m_Sas);
    const std::string Info = BuildSasInfoLocked();
    ByteArray EmojiBytes(6);
    if(olm_sas_generate_bytes(Sas, Info.data(), Info.size(), EmojiBytes.data(), EmojiBytes.size()) != 0) {
        FailLocked("Could not derive the comparison code");
        return false;
    }
    ByteArray DecimalBytes(5);
    if(olm_sas_generate_bytes(Sas, Info.data(), Info.size(), DecimalBytes.data(), DecimalBytes.size()) != 0) {
        FailLocked("Could not derive the comparison code");
        return false;
    }
    // Spec "SAS method: emoji": first 42 bits split into 7 groups of 6 bits,
    // each group indexing the 64-entry emoji table.
    std::vector<int> EmojiIndexes;
    EmojiIndexes.reserve(7);
    for(unsigned Group = 0; Group < 7; ++Group) {
        const unsigned Bit = 6U * Group;
        const std::uint32_t Window = static_cast<std::uint32_t>((EmojiBytes[Bit / 8] << 8) | EmojiBytes[Bit / 8 + 1]);
        EmojiIndexes.push_back(static_cast<int>((Window << (Bit % 8)) >> 10) & 0x3F);
    }
    // Spec "SAS method: decimal": three 13-bit slices of 5 bytes, each +1000.
    const std::uint8_t *B = DecimalBytes.data();
    std::vector<int> DecimalValues = {
        ((B[0] << 5) | (B[1] >> 3)) + 1000,
        (((B[1] & 0x7) << 10) | (B[2] << 2) | (B[3] >> 6)) + 1000,
        (((B[3] & 0x3F) << 7) | (B[4] >> 1)) + 1000,
    };
    {
        std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
        m_Snapshot.EmojiIndexes = std::move(EmojiIndexes);
        m_Snapshot.DecimalValues = std::move(DecimalValues);
    }
    return true;
}

// Both ephemeral keys in hand: check the commitment (we are the start sender),
// feed their key to libolm, answer with ours when we accepted their start and
// derive the emoji/decimal sequences.
void VerificationManager::TryFinishKeyExchangeLocked() {
    if(CurrentStepLocked() != VerificationStep::Started || m_OurSasPub.empty() || m_TheirSasKey.empty()) {
        return;
    }
    if(m_WeSentStart && m_TheirCommitment.empty()) {
        return; // their accept (carrying the commitment) has not arrived yet
    }
    if(m_WeSentStart) {
        // The commitment binds their ephemeral key to our start content, so a
        // MitM rewriting either side of the exchange shows up right here.
        const std::string Expected = UnpaddedBase64(Crypto::Sha256(m_TheirSasKey + Util::CanonicalJson(m_StartContent)));
        if(Expected != m_TheirCommitment) {
            SendCancelLocked("m.mismatched_commitment", "The ephemeral key does not match the commitment");
            FailLocked("The other device sent a mismatched key");
            return;
        }
    }
    auto *Sas = static_cast<OlmSAS *>(m_Sas);
    std::vector<std::uint8_t> KeyBytes(m_TheirSasKey.begin(), m_TheirSasKey.end());
    if(KeyBytes.size() != olm_sas_pubkey_length(Sas) || olm_sas_set_their_key(Sas, KeyBytes.data(), KeyBytes.size()) != 0) {
        SendCancelLocked("m.invalid_message", "The ephemeral key is not valid unpadded base64");
        FailLocked("The other device sent an invalid ephemeral key");
        return;
    }
    if(!m_WeSentStart) {
        // Spec step 10: the accept-sender reveals its key only after receiving
        // the start-sender's key.
        std::string UserId;
        std::string DeviceId;
        std::string TransactionId;
        GetPeerLocked(UserId, DeviceId, TransactionId);
        nlohmann::json Key;
        Key["transaction_id"] = TransactionId;
        Key["key"] = m_OurSasPub;
        if(!SendVerification(UserId, DeviceId, "m.key.verification.key", Key)) {
            FailLocked("Could not send our ephemeral key");
            return;
        }
    }
    if(ComputeSasLocked()) {
        SetStepLocked(VerificationStep::EmojisReady);
    }
}

// --- MAC round ---

void VerificationManager::VerifyPendingMacLocked() {
    if(!m_PendingMac.is_object() || m_PendingMac.empty() || m_Sas == nullptr) {
        return;
    }
    std::string TheirUser;
    std::string TheirDevice;
    std::string TransactionId;
    GetPeerLocked(TheirUser, TheirDevice, TransactionId);
    const std::string OurUser = m_Api.GetUserId();
    const std::string OurDevice = m_Api.GetDeviceId();
    const std::string ExpectedKeyId = "ed25519:" + TheirDevice;
    const nlohmann::json Empty;
    const nlohmann::json &MacMap = m_PendingMac.contains("mac") ? m_PendingMac["mac"] : Empty;

    auto Mismatch = [&](const std::string &Why) {
        m_PendingMac = nlohmann::json::object();
        SendCancelLocked("m.key_mismatch", Why);
        FailLocked(Why);
    };

    if(!MacMap.is_object() || MacMap.empty() || !MacMap.contains(ExpectedKeyId) || !m_PendingMac.contains("keys")) {
        Mismatch("The other device sent an incomplete verification MAC");
        return;
    }
    // The other device's ed25519 key: verification itself needs no prior key
    // share, so query the device list lazily here.
    std::vector<DeviceIdentity> Devices = m_Devices.GetDevices(TheirUser);
    const DeviceIdentity *Their = nullptr;
    auto FindThem = [&]() -> const DeviceIdentity * {
        for(const DeviceIdentity &Device : Devices) {
            if(Device.DeviceId == TheirDevice) {
                return &Device;
            }
        }
        return nullptr;
    };
    Their = FindThem();
    if(Their == nullptr) {
        m_Devices.QueryKeys(m_Api, {TheirUser});
        Devices = m_Devices.GetDevices(TheirUser);
        Their = FindThem();
    }
    if(Their == nullptr || Their->Ed25519Key.empty()) {
        Mismatch("The other device's signing key is unavailable");
        return;
    }
    // Spec "MAC calculation": recompute the MAC over the sorted, comma-joined
    // key-id list taken straight from the message, then the per-key MACs.
    std::vector<std::string> KeyIds;
    KeyIds.reserve(MacMap.size());
    for(auto It = MacMap.begin(); It != MacMap.end(); ++It) {
        KeyIds.push_back(It.key());
    }
    std::sort(KeyIds.begin(), KeyIds.end());
    // The MAC info is a RAW concatenation with NO separators - verified
    // byte-for-byte against the spec wording, matrix-js-sdk (_sendMAC/_checkMAC)
    // and matrix-rust-sdk (verification/sas/helpers.rs). Adding "|" here breaks
    // interop with Element on both sides of the MAC round.
    const std::string InfoBase = "MATRIX_KEY_VERIFICATION_MAC" + TheirUser + TheirDevice + OurUser + OurDevice + TransactionId;
    const std::string ExpectedKeys = CalculateMacLocked(Util::Join(KeyIds, ","), InfoBase + "KEY_IDS");
    if(!ConstantTimeEqual(ExpectedKeys, Util::JsonGetString(m_PendingMac, "keys"))) {
        Mismatch("The other device sent a mismatched key-list MAC");
        return;
    }
    // Only keys we hold a copy of can be value-checked; for same-user device
    // verification that is the device ed25519 key (anything else, e.g. a
    // master signing key, is still bound by the keys MAC above).
    const std::string ExpectedMac = CalculateMacLocked(Their->Ed25519Key, InfoBase + ExpectedKeyId);
    if(!ConstantTimeEqual(ExpectedMac, MacMap[ExpectedKeyId].get<std::string>())) {
        Mismatch("The other device sent a mismatched key MAC");
        return;
    }
    m_PendingMac = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
        const std::string Slot = TheirUser + "|" + TheirDevice;
        if(std::find(m_VerifiedDevices.begin(), m_VerifiedDevices.end(), Slot) == m_VerifiedDevices.end()) {
            m_VerifiedDevices.push_back(Slot);
        }
        m_Snapshot.Step = VerificationStep::Verified;
        m_Snapshot.Error.clear();
    }
    // done only announces that we finished; the recorded trust stands even if
    // this last send fails. Cross-signing promotion happens in
    // VerificationLayout - it is the component wired to CrossSigningManager.
    nlohmann::json Done;
    Done["transaction_id"] = TransactionId;
    (void)SendVerification(TheirUser, TheirDevice, "m.key.verification.done", Done); // best effort
}

// --- UI-driven operations (blocking: run them on Core::JobQueue) ---

bool VerificationManager::Start(const std::string &UserId, const std::string &DeviceId) {
    if(UserId.empty() || DeviceId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(IsBusy()) {
        return false; // one transaction at a time
    }
    ResetTransactionLocked();
    const std::string TransactionId = MakeHttpTransactionId();
    nlohmann::json Content;
    Content["from_device"] = m_Api.GetDeviceId();
    Content["transaction_id"] = TransactionId;
    Content["methods"] = nlohmann::json::array({SasMethod});
    Content["timestamp"] = Util::NowMs();
    {
        std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
        m_Snapshot.Step = VerificationStep::RequestSent;
        m_Snapshot.OtherUserId = UserId;
        m_Snapshot.OtherDeviceId = DeviceId;
        m_Snapshot.TransactionId = TransactionId;
        m_Snapshot.EmojiIndexes.clear();
        m_Snapshot.DecimalValues.clear();
        m_Snapshot.Error.clear();
    }
    if(!SendVerification(UserId, DeviceId, "m.key.verification.request", Content)) {
        FailLocked("Could not send the verification request");
        return false;
    }
    return true;
}

bool VerificationManager::AcceptIncoming() {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(CurrentStepLocked() != VerificationStep::RequestReceived) {
        return false;
    }
    if(!Contains(m_TheirMethods, SasMethod)) {
        // The user accepted a request we cannot serve; cancel it instead of
        // leaving the other device waiting (spec m.unknown_method).
        SendCancelLocked("m.unknown_method", "The device does not offer m.sas.v1");
        FailLocked("The other device does not offer m.sas.v1");
        return false;
    }
    std::string UserId;
    std::string DeviceId;
    std::string TransactionId;
    GetPeerLocked(UserId, DeviceId, TransactionId);
    nlohmann::json Content;
    Content["from_device"] = m_Api.GetDeviceId();
    Content["transaction_id"] = TransactionId;
    Content["methods"] = nlohmann::json::array({SasMethod});
    if(!SendVerification(UserId, DeviceId, "m.key.verification.ready", Content)) {
        FailLocked("Could not answer the verification request");
        return false;
    }
    SetStepLocked(VerificationStep::Started); // waiting for their m.key.verification.start
    return true;
}

bool VerificationManager::RejectIncoming() {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(CurrentStepLocked() != VerificationStep::RequestReceived) {
        return false;
    }
    SendCancelLocked("m.user", "The verification request was declined");
    std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
    m_Snapshot.Step = VerificationStep::Cancelled;
    m_Snapshot.Error = "Request declined";
    return true;
}

bool VerificationManager::ConfirmEmojisMatch() {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(CurrentStepLocked() != VerificationStep::EmojisReady) {
        return false;
    }
    std::string TheirUser;
    std::string TheirDevice;
    std::string TransactionId;
    GetPeerLocked(TheirUser, TheirDevice, TransactionId);
    // Spec "MAC calculation": one MAC per key we want verified plus a MAC over
    // the sorted key-id list; the info segments are raw-concatenated WITHOUT
    // separators: MATRIX_KEY_VERIFICATION_MAC<user whose key><sending
    // device><other user><receiving device><txn><key id or KEY_IDS> - see the
    // verified note in VerifyPendingMacLocked before ever touching this.
    const std::string InfoBase =
        "MATRIX_KEY_VERIFICATION_MAC" + m_Api.GetUserId() + m_Api.GetDeviceId() + TheirUser + TheirDevice + TransactionId;
    const std::string KeyId = "ed25519:" + m_Api.GetDeviceId();
    const std::string KeyMac = CalculateMacLocked(m_Account.GetIdentity().Ed25519Key, InfoBase + KeyId);
    const std::string KeysMac = CalculateMacLocked(KeyId, InfoBase + "KEY_IDS");
    if(KeyMac.empty() || KeysMac.empty()) {
        FailLocked("Could not calculate the verification MAC");
        return false;
    }
    nlohmann::json Content;
    Content["transaction_id"] = TransactionId;
    Content["mac"] = nlohmann::json{{KeyId, KeyMac}};
    Content["keys"] = KeysMac;
    if(!SendVerification(TheirUser, TheirDevice, "m.key.verification.mac", Content)) {
        FailLocked("Could not send the verification MAC");
        return false;
    }
    m_Confirmed = true;
    SetStepLocked(VerificationStep::AcceptedByUs);
    if(m_PendingMac.is_object() && !m_PendingMac.empty()) {
        VerifyPendingMacLocked(); // their MAC raced ahead of our confirmation
    }
    return true;
}

bool VerificationManager::RejectEmojis() {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(CurrentStepLocked() != VerificationStep::EmojisReady) {
        return false;
    }
    SendCancelLocked("m.mismatched_sas", "The short authentication strings did not match");
    FailLocked("Emojis did not match");
    return true;
}

void VerificationManager::Cancel() {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(!IsActiveStep(CurrentStepLocked())) {
        return;
    }
    SendCancelLocked("m.user", "The verification was cancelled");
    std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
    m_Snapshot.Step = VerificationStep::Cancelled;
    m_Snapshot.Error.clear();
}

// --- to-device routing (sync thread; may block on sends) ---

void VerificationManager::HandleRequestLocked(const std::string &Sender, const nlohmann::json &Content, const std::string &TransactionId) {
    const std::string TheirDevice = Util::JsonGetString(Content, "from_device");
    if(TheirDevice.empty()) {
        return;
    }
    const VerificationStep Step = CurrentStepLocked();
    if(IsActiveStep(Step)) {
        std::string PendingUser;
        std::string PendingDevice;
        std::string OurTxn;
        GetPeerLocked(PendingUser, PendingDevice, OurTxn);
        if(TransactionId == OurTxn) {
            // Duplicate of the pending request; re-publish it below.
        } else if(Step == VerificationStep::RequestSent && TheirDevice < m_Api.GetDeviceId()) {
            // Two requests crossed: the lexicographically smaller device
            // adopts the other's request so exactly one side becomes the
            // responder (mirrors the spec's simultaneous-start tie-break).
            ResetTransactionLocked();
        } else {
            nlohmann::json Cancel;
            Cancel["transaction_id"] = TransactionId;
            Cancel["code"] = "m.user";
            Cancel["reason"] = "This device is already running a verification";
            (void)SendVerification(Sender, TheirDevice, "m.key.verification.cancel", Cancel);
            return;
        }
    }
    ResetTransactionLocked();
    m_TheirMethods = GetStringList(Content, "methods");
    std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
    m_Snapshot.Step = VerificationStep::RequestReceived;
    m_Snapshot.OtherUserId = Sender;
    m_Snapshot.OtherDeviceId = TheirDevice;
    m_Snapshot.TransactionId = TransactionId;
    m_Snapshot.EmojiIndexes.clear();
    m_Snapshot.DecimalValues.clear();
    m_Snapshot.Error.clear();
}

void VerificationManager::HandleReadyLocked(const std::string &Sender, const nlohmann::json &Content, const std::string &TransactionId) {
    std::string TheirUser;
    std::string TheirDevice;
    std::string OurTxn;
    GetPeerLocked(TheirUser, TheirDevice, OurTxn);
    if(CurrentStepLocked() != VerificationStep::RequestSent || Sender != TheirUser) {
        return; // duplicate or unrelated ready
    }
    const std::string ReadyDevice = Util::JsonGetString(Content, "from_device");
    if(!ReadyDevice.empty() && ReadyDevice != TheirDevice) {
        std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
        m_Snapshot.OtherDeviceId = ReadyDevice;
        TheirDevice = ReadyDevice;
    }
    if(!Contains(GetStringList(Content, "methods"), SasMethod)) {
        SendCancelLocked("m.unknown_method", "No common verification method");
        FailLocked("The other device does not offer m.sas.v1");
        return;
    }
    // We are the start sender (spec: either side may start; this client starts
    // as soon as the peer accepts the request).
    nlohmann::json Start;
    Start["from_device"] = m_Api.GetDeviceId();
    Start["transaction_id"] = TransactionId;
    Start["method"] = SasMethod;
    Start["key_agreement_protocols"] = nlohmann::json::array({KeyAgreement});
    Start["hashes"] = nlohmann::json::array({"sha256"});
    Start["message_authentication_codes"] = nlohmann::json::array({MacMethodV2, MacMethodV1});
    Start["short_authentication_string"] = nlohmann::json::array({"emoji", "decimal"});
    m_StartContent = Start; // kept verbatim: the peer commits to a hash of this exact content
    m_WeSentStart = true;
    if(!SendVerification(TheirUser, TheirDevice, "m.key.verification.start", Start)) {
        m_WeSentStart = false;
        FailLocked("Could not send the verification start");
        return;
    }
    SetStepLocked(VerificationStep::Started);
}

void VerificationManager::HandleStartLocked(const std::string &Sender, const nlohmann::json &Content, const std::string &TransactionId) {
    std::string TheirUser;
    std::string TheirDevice;
    std::string OurTxn;
    GetPeerLocked(TheirUser, TheirDevice, OurTxn);
    const VerificationStep Step = CurrentStepLocked();
    const bool Ours = !OurTxn.empty() && TransactionId == OurTxn;

    if(Util::JsonGetString(Content, "method") != SasMethod) {
        const std::string StartDevice = Util::JsonGetString(Content, "from_device");
        if(!StartDevice.empty()) {
            nlohmann::json Cancel;
            Cancel["transaction_id"] = TransactionId;
            Cancel["code"] = "m.unknown_method";
            Cancel["reason"] = "Only m.sas.v1 is supported";
            (void)SendVerification(Sender, StartDevice, "m.key.verification.cancel", Cancel);
        }
        if(Ours && IsActiveStep(Step)) {
            FailLocked("The other device requested an unsupported method");
        }
        return;
    }
    const std::string StartDevice = Util::JsonGetString(Content, "from_device");
    if(StartDevice.empty()) {
        return;
    }

    // Simultaneous starts: both sides keep the start from the lexicographically
    // smaller device (user ids first, device ids for same-user verification)
    // and drop their own - so the smaller side stays the start sender and the
    // larger one flips to accepting.
    if(m_WeSentStart && Ours && IsActiveStep(Step)) {
        if(StartDevice > m_Api.GetDeviceId()) {
            return; // their start loses the tie-break; ours is already on its way
        }
        m_WeSentStart = false; // our start loses: accept theirs instead
    } else if(Ours && !IsActiveStep(Step)) {
        return; // a finished transaction stays finished
    } else if(!Ours) {
        if(IsActiveStep(Step)) {
            return; // busy elsewhere; start is exempt from the unknown-transaction cancel rule
        }
        // Deprecated direct start without a request: adopt it, take the
        // accepting side (spec says clients must still handle these).
        ResetTransactionLocked();
        std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
        m_Snapshot.Step = VerificationStep::Started;
        m_Snapshot.OtherUserId = Sender;
        m_Snapshot.OtherDeviceId = StartDevice;
        m_Snapshot.TransactionId = TransactionId;
        m_Snapshot.EmojiIndexes.clear();
        m_Snapshot.DecimalValues.clear();
        m_Snapshot.Error.clear();
    }

    auto UnknownMethod = [&](const std::string &Why) {
        SendCancelLocked("m.unknown_method", Why);
        FailLocked(Why);
    };
    if(!Contains(GetStringList(Content, "key_agreement_protocols"), KeyAgreement)) {
        UnknownMethod("No common key agreement protocol");
        return;
    }
    if(!Contains(GetStringList(Content, "hashes"), "sha256")) {
        UnknownMethod("No common hash method");
        return;
    }
    std::string Mac;
    if(Contains(GetStringList(Content, "message_authentication_codes"), MacMethodV2)) {
        Mac = MacMethodV2;
    } else if(Contains(GetStringList(Content, "message_authentication_codes"), MacMethodV1)) {
        Mac = MacMethodV1;
    } else {
        UnknownMethod("No common message authentication code");
        return;
    }
    const std::vector<std::string> TheirSas = GetStringList(Content, "short_authentication_string");
    std::vector<std::string> ChosenSas;
    if(Contains(TheirSas, "emoji")) {
        ChosenSas.emplace_back("emoji");
    }
    if(Contains(TheirSas, "decimal")) {
        ChosenSas.emplace_back("decimal");
    }
    if(ChosenSas.empty()) {
        UnknownMethod("No common comparison method");
        return;
    }
    m_MacMethod = Mac;

    // Accepting side: create our ephemeral key and commit to it before the
    // peer ever sees it (commitment = unpadded base64 SHA-256 of our key
    // concatenated with the canonical JSON of their start content).
    if(!EnsureSasLocked()) {
        FailLocked("Could not initialize the verification");
        return;
    }
    const std::string CanonicalStart = Util::CanonicalJson(Content);
    nlohmann::json Accept;
    Accept["transaction_id"] = TransactionId;
    Accept["method"] = SasMethod;
    Accept["key_agreement_protocol"] = KeyAgreement;
    Accept["hash"] = "sha256";
    Accept["message_authentication_code"] = Mac;
    Accept["short_authentication_string"] = ChosenSas;
    Accept["commitment"] = UnpaddedBase64(Crypto::Sha256(m_OurSasPub + CanonicalStart));
    if(!SendVerification(Sender, StartDevice, "m.key.verification.accept", Accept)) {
        FailLocked("Could not send the verification acceptance");
        return;
    }
    SetStepLocked(VerificationStep::Started);
}

void VerificationManager::HandleAcceptLocked(const nlohmann::json &Content, const std::string &TransactionId) {
    if(CurrentStepLocked() != VerificationStep::Started || !m_WeSentStart) {
        return; // only the start sender consumes an accept
    }
    const std::string Agreement = Util::JsonGetString(Content, "key_agreement_protocol");
    const std::string Hash = Util::JsonGetString(Content, "hash");
    const std::string Mac = Util::JsonGetString(Content, "message_authentication_code");
    const std::string Commitment = Util::JsonGetString(Content, "commitment");
    if(Agreement != KeyAgreement || Hash != "sha256" || (Mac != MacMethodV2 && Mac != MacMethodV1)) {
        SendCancelLocked("m.unknown_method", "The chosen verification parameters are not supported");
        FailLocked("The other device chose unsupported verification parameters");
        return;
    }
    if(Commitment.empty()) {
        SendCancelLocked("m.invalid_message", "The acceptance has no key commitment");
        FailLocked("The other device sent an invalid acceptance");
        return;
    }
    if(!EnsureSasLocked()) {
        FailLocked("Could not initialize the verification");
        return;
    }
    m_TheirCommitment = Commitment;
    m_MacMethod = Mac;
    // Spec step 9: the start sender reveals its ephemeral key now and verifies
    // the commitment when the accept-sender's key arrives.
    std::string UserId;
    std::string DeviceId;
    std::string Transaction;
    GetPeerLocked(UserId, DeviceId, Transaction);
    nlohmann::json Key;
    Key["transaction_id"] = TransactionId;
    Key["key"] = m_OurSasPub;
    if(!SendVerification(UserId, DeviceId, "m.key.verification.key", Key)) {
        FailLocked("Could not send our ephemeral key");
        return;
    }
    TryFinishKeyExchangeLocked();
}

void VerificationManager::HandleKeyLocked(const nlohmann::json &Content, const std::string &) {
    if(CurrentStepLocked() != VerificationStep::Started) {
        return;
    }
    const std::string TheirKey = Util::JsonGetString(Content, "key");
    if(TheirKey.empty()) {
        SendCancelLocked("m.invalid_message", "The key message has no key");
        FailLocked("The other device sent an invalid key message");
        return;
    }
    m_TheirSasKey = TheirKey; // buffered: may arrive before their accept under reordering
    TryFinishKeyExchangeLocked();
}

void VerificationManager::HandleMacLocked(const nlohmann::json &Content, const std::string &) {
    const VerificationStep Step = CurrentStepLocked();
    if(Step != VerificationStep::EmojisReady && Step != VerificationStep::AcceptedByUs) {
        return; // out of sequence; dropped rather than answered to avoid error loops
    }
    // Only becomes trusted once our user confirmed the emojis; until then it
    // just sits here (their mac may legitimately arrive first).
    m_PendingMac = Content;
    if(m_Confirmed) {
        VerifyPendingMacLocked();
    }
}

void VerificationManager::HandleToDevice(const nlohmann::json &Event) {
    if(!Event.is_object() || !Event.contains("content") || !Event["content"].is_object()) {
        return;
    }
    const std::string Type = Util::JsonGetString(Event, "type");
    const std::string Sender = Util::JsonGetString(Event, "sender");
    const nlohmann::json &Content = Event["content"];
    const std::string TransactionId = Util::JsonGetString(Content, "transaction_id");
    if(Sender.empty() || TransactionId.empty()) {
        return; // this flow is to-device only, where transaction_id is required
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);

    std::string TheirUser;
    std::string TheirDevice;
    std::string OurTxn;
    GetPeerLocked(TheirUser, TheirDevice, OurTxn);
    const bool Ours = !OurTxn.empty() && TransactionId == OurTxn;

    if(Type == "m.key.verification.request") {
        HandleRequestLocked(Sender, Content, TransactionId);
    } else if(Type == "m.key.verification.ready") {
        if(Ours) {
            HandleReadyLocked(Sender, Content, TransactionId);
        }
    } else if(Type == "m.key.verification.start") {
        HandleStartLocked(Sender, Content, TransactionId);
    } else if(Type == "m.key.verification.accept") {
        if(Ours) {
            HandleAcceptLocked(Content, TransactionId);
        }
    } else if(Type == "m.key.verification.key") {
        if(Ours) {
            HandleKeyLocked(Content, TransactionId);
        }
    } else if(Type == "m.key.verification.mac") {
        if(Ours) {
            HandleMacLocked(Content, TransactionId);
        }
    } else if(Type == "m.key.verification.done") {
        // Trust is settled by the MAC round; their done only acknowledges ours.
    } else if(Type == "m.key.verification.reject" || Type == "m.key.verification.cancel") {
        if(Ours && IsActiveStep(CurrentStepLocked())) {
            const std::string Code =
                Type == "m.key.verification.cancel" ? Util::JsonGetString(Content, "code", "m.user") : std::string("m.user");
            std::lock_guard<std::mutex> SnapLock(m_SnapshotMutex);
            m_Snapshot.Step = VerificationStep::Cancelled;
            m_Snapshot.Error = Code;
        }
    }
    // Events for foreign transactions are ignored on purpose: replying
    // m.unknown_transaction here would risk cancel loops over trailing events
    // of finished handshakes (the spec explicitly warns about error loops).
}

bool VerificationManager::IsVerified(const std::string &UserId, const std::string &DeviceId) const {
    const std::string Slot = UserId + "|" + DeviceId;
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    return std::find(m_VerifiedDevices.begin(), m_VerifiedDevices.end(), Slot) != m_VerifiedDevices.end();
}

nlohmann::json VerificationManager::ExportState() const {
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    return nlohmann::json{{"verified", m_VerifiedDevices}};
}

void VerificationManager::ImportState(const nlohmann::json &State) {
    std::vector<std::string> Verified;
    if(State.is_object()) {
        const auto It = State.find("verified");
        if(It != State.end() && It->is_array()) {
            for(const nlohmann::json &Item : *It) {
                if(Item.is_string()) {
                    Verified.push_back(Item.get<std::string>());
                }
            }
        }
    }
    std::lock_guard<std::mutex> Lock(m_SnapshotMutex);
    m_VerifiedDevices = std::move(Verified);
}

} // namespace NxMatrix::E2ee