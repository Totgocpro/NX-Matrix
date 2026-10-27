// E2EE orchestration: account lifecycle, key upload, room key sharing, to-device dispatch.
#include "NxMatrix/E2ee/E2eeManager.hpp"

#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/E2ee/DeviceTracker.hpp"
#include "NxMatrix/E2ee/MegolmManager.hpp"
#include "NxMatrix/E2ee/OlmAccount.hpp"
#include "NxMatrix/E2ee/OlmSessionManager.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <set>

namespace NxMatrix::E2ee {

namespace {

constexpr const char *MegolmAlgorithm = "m.megolm.v1.aes-sha2";
constexpr const char *OlmAlgorithm = "m.olm.v1.curve25519-aes-sha2";
constexpr std::int64_t KeyRequestCooldownMs = 5 * 60 * 1000; // one m.room_key.request per session id per 5 min
constexpr std::size_t OneTimeKeysPerUpload = 100;
constexpr std::size_t MinimumOneTimeKeysDefault = 10;

// Random hex string for to-device transaction ids / key request ids.
std::string MakeTransactionId() {
    static const char *Hex = "0123456789abcdef";
    const ByteArray Bytes = Crypto::RandomBytes(16);
    std::string Out;
    Out.reserve(Bytes.size() * 2);
    for(std::uint8_t Byte : Bytes) {
        Out.push_back(Hex[Byte >> 4]);
        Out.push_back(Hex[Byte & 0x0F]);
    }
    return Out;
}

std::string DeviceSlot(const DeviceIdentity &Device) {
    return Device.UserId + "|" + Device.DeviceId;
}

} // namespace

E2eeManager::E2eeManager(Matrix::Client &Api) : m_Api(Api) {
    m_Account = new OlmAccount();
    m_Sessions = new OlmSessionManager();
    m_Megolm = new MegolmManager();
    m_Devices = new DeviceTracker();
    // The session manager needs the account from the first to-device message onward
    // (inbound session creation), long before any /keys/claim runs.
    m_Sessions->m_Account = m_Account;
    m_Verification = new VerificationManager(Api, *m_Account, *m_Sessions, *m_Devices);
    m_CrossSigning = new CrossSigningManager(Api, *m_Account);
    m_Backup = new KeyBackup(Api, *m_Megolm);
    // Same pattern as OlmSessionManager: the backup signs its auth_data with
    // our ed25519 device key, so other clients can trust the backup version.
    m_Backup->m_Account = m_Account;
}

VerificationManager &E2eeManager::GetVerification() {
    return *m_Verification;
}

CrossSigningManager &E2eeManager::GetCrossSigning() {
    return *m_CrossSigning;
}

KeyBackup &E2eeManager::GetKeyBackup() {
    return *m_Backup;
}

DeviceTracker &E2eeManager::GetDevices() {
    return *m_Devices;
}

E2eeManager::~E2eeManager() {
    delete m_Backup;
    m_Backup = nullptr;
    delete m_CrossSigning;
    m_CrossSigning = nullptr;
    delete m_Verification;
    m_Verification = nullptr;
    delete m_Devices;
    m_Devices = nullptr;
    delete m_Megolm;
    m_Megolm = nullptr;
    delete m_Sessions;
    m_Sessions = nullptr;
    delete m_Account;
    m_Account = nullptr;
}

void E2eeManager::SetIdentity(const std::string &UserId, const std::string &DeviceId) {
    m_UserId = UserId;
    m_DeviceId = DeviceId;
    if(m_Account == nullptr || !m_Account->IsValid()) {
        return; // identity keys arrive once the account is created or imported
    }
    const DeviceIdentity &Identity = m_Account->GetIdentity();
    if(Identity.UserId != UserId || Identity.DeviceId != DeviceId) {
        // The loaded account belongs to a previous login: invalidate it so the next
        // EnsureKeysUploaded creates a fresh identity instead of uploading stale keys.
        m_Account->m_Valid = false;
        m_Account->m_PublishedKeyCount = 0;
        m_KeysUploaded = false;
        m_LastOneTimeKeyCount = 0;
        return;
    }
    ApplyIdentity();
}

void E2eeManager::SetMasterKey(const Crypto::KeyBytes &MasterKey) {
    m_MasterKey = MasterKey;
    m_MasterKeySet = true;
    ApplyPickleKey();
}

bool E2eeManager::IsReady() const {
    return m_Account != nullptr && m_Account->IsValid() && m_MasterKeySet && !m_UserId.empty() && !m_DeviceId.empty();
}

void E2eeManager::ApplyPickleKey() {
    if(!m_MasterKeySet) {
        return;
    }
    // One key for every pickle in the state: account, olm sessions and megolm sessions.
    m_Account->m_PickleKey = m_MasterKey;
    m_Sessions->m_PickleKey = m_MasterKey;
    m_Megolm->m_PickleKey = m_MasterKey;
}

void E2eeManager::ApplyIdentity() {
    if(m_Account == nullptr || !m_Account->IsValid()) {
        return;
    }
    const DeviceIdentity Identity = m_Account->GetIdentity();
    m_Sessions->m_OurIdentity = Identity;
    m_Megolm->m_OurIdentity = Identity;
}

nlohmann::json E2eeManager::BuildOlmPayload(const std::string &Type, const nlohmann::json &Content, const DeviceIdentity &Target) const {
    const DeviceIdentity &Ours = m_Account->GetIdentity();
    nlohmann::json Payload;
    Payload["type"] = Type;
    Payload["content"] = Content;
    Payload["sender"] = Ours.UserId;
    Payload["recipient"] = Target.UserId;
    Payload["keys"] = nlohmann::json{{"ed25519", Ours.Ed25519Key}};
    Payload["recipient_keys"] = nlohmann::json{{"ed25519", Target.Ed25519Key}};
    return Payload;
}

nlohmann::json E2eeManager::ExportState() const {
    nlohmann::json State;
    State["version"] = 1;
    State["olm_account"] = m_Account->ExportState();
    State["olm_sessions"] = m_Sessions->ExportState();
    State["megolm"] = m_Megolm->ExportState();
    State["devices"] = m_Devices->ExportState();
    State["verification"] = m_Verification->ExportState();
    State["cross_signing"] = m_CrossSigning->ExportState();
    State["key_backup"] = m_Backup->ExportState();
    State["last_otk_count"] = static_cast<std::int64_t>(m_LastOneTimeKeyCount);
    State["keys_uploaded"] = m_KeysUploaded;
    return State;
}

void E2eeManager::ImportState(const nlohmann::json &State) {
    if(!State.is_object()) {
        return;
    }
    const std::int64_t Version = Util::JsonGetInt(State, "version", 1);
    if(Version != 1) {
        return; // a future state format we cannot interpret: keep the current (empty) state
    }

    // Missing sections are tolerated. Pickled sections only open with the master key,
    // so SetMasterKey must run before ImportState — otherwise they degrade to a fresh
    // account/sessions on the next EnsureKeysUploaded.
    if(State.contains("olm_account") && State["olm_account"].is_object()) {
        if(m_Account->ImportState(State["olm_account"], m_MasterKey)) {
            ApplyIdentity();
        }
    }
    if(State.contains("olm_sessions") && State["olm_sessions"].is_object()) {
        m_Sessions->ImportState(State["olm_sessions"], m_MasterKey);
    }
    if(State.contains("megolm") && State["megolm"].is_object()) {
        m_Megolm->ImportState(State["megolm"], m_MasterKey);
    }
    if(State.contains("devices")) {
        m_Devices->ImportState(State["devices"]);
    }
    if(State.contains("verification")) {
        m_Verification->ImportState(State["verification"]);
    }
    if(State.contains("cross_signing")) {
        m_CrossSigning->ImportState(State["cross_signing"]);
    }
    if(State.contains("key_backup")) {
        m_Backup->ImportState(State["key_backup"]);
    }
    const std::int64_t StoredCount = Util::JsonGetInt(State, "last_otk_count", 0);
    m_LastOneTimeKeyCount = StoredCount > 0 ? static_cast<std::size_t>(StoredCount) : 0;
    if(m_Account->IsValid()) {
        m_Account->m_PublishedKeyCount = m_LastOneTimeKeyCount;
    }
    m_KeysUploaded = Util::JsonGetBool(State, "keys_uploaded", false);
}

bool E2eeManager::EnsureKeysUploaded(std::size_t MinimumOneTimeKeys) {
    if(m_Account == nullptr) {
        return false;
    }
    if(!m_Account->IsValid()) {
        if(m_UserId.empty() || m_DeviceId.empty() || !m_MasterKeySet) {
            return false; // identity or pickle key not established yet: nothing can be created
        }
        if(!m_Account->CreateNew(m_UserId, m_DeviceId)) {
            return false;
        }
        ApplyIdentity();
        m_KeysUploaded = false;
        m_LastOneTimeKeyCount = 0;
        m_Account->m_PublishedKeyCount = 0;
    }

    const bool FirstUpload = !m_KeysUploaded;
    if(!FirstUpload && m_LastOneTimeKeyCount >= MinimumOneTimeKeys) {
        return true; // the server still has enough one-time keys
    }

    nlohmann::json Body;
    if(FirstUpload) {
        const nlohmann::json DeviceKeys = m_Account->BuildDeviceKeys();
        if(!DeviceKeys.is_object() || DeviceKeys.empty()) {
            return false;
        }
        Body["device_keys"] = DeviceKeys;
    }
    const std::map<std::string, nlohmann::json> OneTimeKeys = m_Account->GenerateSignedOneTimeKeys(OneTimeKeysPerUpload);
    if(OneTimeKeys.empty()) {
        return false;
    }
    Body["one_time_keys"] = OneTimeKeys;

    auto Response = m_Api.UploadKeys(Body);
    if(!Response.Ok) {
        return false; // swallowed: retried on the next sync or send
    }
    // Refresh our view of the server's pool; libolm keeps no published count itself.
    std::int64_t Count = 0;
    if(Response.Value.contains("one_time_key_counts") && Response.Value["one_time_key_counts"].is_object()) {
        const nlohmann::json &Counts = Response.Value["one_time_key_counts"];
        Count = Util::JsonGetInt(Counts, "signed_curve25519", 0);
        if(Count == 0) {
            Count = Util::JsonGetInt(Counts, "curve25519", 0);
        }
    }
    if(Count < 0) {
        Count = 0;
    }
    m_LastOneTimeKeyCount = static_cast<std::size_t>(Count);
    m_Account->m_PublishedKeyCount = m_LastOneTimeKeyCount;
    m_KeysUploaded = true;
    return true;
}

void E2eeManager::ShareRoomKey(const std::string &RoomId, const std::vector<DeviceIdentity> &Targets) {
    if(m_Account == nullptr || !m_Account->IsValid() || Targets.empty()) {
        return;
    }
    const nlohmann::json RoomKey = m_Megolm->BuildRoomKeyMessage(RoomId);
    if(!RoomKey.is_object() || RoomKey.empty()) {
        return;
    }
    const std::string SessionId = Util::JsonGetString(RoomKey, "session_id");
    if(SessionId.empty()) {
        return;
    }

    // Track what already received the current session: rotation or new devices force a
    // resend, everything else is skipped so repeated events do not re-spam the room key.
    const auto SharedIdIt = m_SharedSessionId.find(RoomId);
    const std::string PreviousSession = SharedIdIt == m_SharedSessionId.end() ? std::string{} : SharedIdIt->second;
    std::set<std::string> Delivered;
    if(PreviousSession == SessionId) {
        const auto It = m_SharedDevices.find(RoomId);
        if(It != m_SharedDevices.end()) {
            Delivered.insert(It->second.begin(), It->second.end());
        }
    }

    nlohmann::json Messages = nlohmann::json::object();
    for(const DeviceIdentity &Device : Targets) {
        if(Device.UserId == m_UserId && Device.DeviceId == m_DeviceId) {
            continue; // we keep an inbound copy of our own session instead (MegolmManager)
        }
        const std::string Slot = DeviceSlot(Device);
        if(PreviousSession == SessionId && Delivered.count(Slot) != 0) {
            continue; // this device already holds the shared session
        }
        if(!m_Sessions->HasSessionFor(Device.Curve25519Key)) {
            continue; // claim failed earlier; retried on the next share
        }
        nlohmann::json Content;
        if(!m_Sessions->EncryptToDevice(Device, BuildOlmPayload("m.room_key", RoomKey, Device), Content)) {
            continue;
        }
        Messages[Device.UserId][Device.DeviceId] = Content;
        Delivered.insert(Slot);
    }
    if(Messages.empty()) {
        return; // nobody new to tell (or no sessions open yet — retried next event)
    }

    if(!m_Api.SendToDevice("m.room.encrypted", Messages, MakeTransactionId()).Ok) {
        return; // tracking untouched, so the next event retries the whole share
    }
    m_SharedSessionId[RoomId] = SessionId;
    m_SharedDevices[RoomId] = std::vector<std::string>(Delivered.begin(), Delivered.end());
}

bool E2eeManager::EncryptRoomEvent(const std::string &RoomId, const std::vector<std::string> &MemberUserIds, const std::string &EventType,
                                   const nlohmann::json &PlainContent, nlohmann::json &OutEncryptedContent, std::string *OutError) {
    const auto Fail = [OutError](const char *Reason) {
        if(OutError != nullptr) {
            *OutError = Reason;
        }
        return false;
    };
    EnsureKeysUploaded(); // best effort: sending still works without fresh one-time keys
    if(m_Account == nullptr || !m_Account->IsValid()) {
        return Fail("Encryption failed: device keys not ready");
    }

    // Refresh only the member device lists that are stale or were never queried.
    const std::vector<std::string> Stale = m_Devices->FilterStale(MemberUserIds);
    if(!Stale.empty()) {
        m_Devices->QueryKeys(m_Api, Stale); // best effort: failures stay dirty and retry
    }
    const std::vector<DeviceIdentity> Members = m_Devices->GetDevicesForUsers(MemberUserIds);
    m_Devices->EnsureOlmSessions(m_Api, *m_Account, *m_Sessions, Members); // best effort per device

    m_Megolm->RotateIfNeeded(RoomId, Util::NowMs()); // a rotation changes the session id -> reshare below
    ShareRoomKey(RoomId, Members);

    if(!m_Megolm->EncryptRoomEvent(RoomId, EventType, PlainContent, OutEncryptedContent)) {
        return Fail("Encryption failed: no megolm session");
    }
    return true;
}

bool E2eeManager::DecryptRoomEvent(const Matrix::Event &EncryptedEvent, nlohmann::json &OutPlainContent, std::string &OutEventType,
                                   bool &OutFailed) {
    OutFailed = false;
    const nlohmann::json &Content = EncryptedEvent.Content;
    const std::string Algorithm = Util::JsonGetString(Content, "algorithm");
    if(Algorithm != MegolmAlgorithm) {
        OutFailed = true; // m.olm.v1 (or unknown) in the room timeline: not decryptable here
        return false;
    }
    const std::string SenderKey = Util::JsonGetString(Content, "sender_key");
    const std::string SessionId = Util::JsonGetString(Content, "session_id");
    const std::string Ciphertext = Util::JsonGetString(Content, "ciphertext");
    if(SenderKey.empty() || SessionId.empty() || Ciphertext.empty()) {
        OutFailed = true;
        return false;
    }

    if(!m_Megolm->CanDecrypt(SenderKey, SessionId) ||
       !m_Megolm->DecryptRoomEvent(SenderKey, SessionId, Ciphertext, OutPlainContent, OutEventType)) {
        // Missing session, or the key we hold starts after this message's index: ask for
        // it (throttled per session) and mark the event undecryptable for now.
        OutFailed = true;
        RequestMissingKey(EncryptedEvent.RoomId, SenderKey, SessionId, MakeTransactionId());
        return false;
    }
    return true;
}

void E2eeManager::OnSync(const std::vector<nlohmann::json> &ToDeviceEvents, const std::vector<std::string> &ChangedDeviceLists,
                         std::size_t OneTimeKeyCount) {
    for(const nlohmann::json &Event : ToDeviceEvents) {
        if(!Event.is_object() || !Event.contains("content") || !Event["content"].is_object()) {
            continue;
        }
        const std::string Type = Util::JsonGetString(Event, "type");
        const std::string Sender = Util::JsonGetString(Event, "sender");
        const nlohmann::json &Content = Event["content"];

        if(Type == "m.room_key_request") {
            // Spec: m.room_key.request travels as an unencrypted to-device event.
            HandleKeyRequest(Sender, Content);
            continue;
        }
        if(Type.rfind("m.key.verification.", 0) == 0) {
            // Verification messages travel unencrypted (olm-wrapped variant below).
            m_Verification->HandleToDevice(Event);
            continue;
        }
        if(Type != "m.room.encrypted" || Util::JsonGetString(Content, "algorithm") != OlmAlgorithm) {
            continue;
        }
        const std::string SenderKey = Util::JsonGetString(Content, "sender_key");
        if(SenderKey.empty()) {
            continue;
        }
        nlohmann::json Inner;
        if(!m_Sessions->DecryptFromDevice(SenderKey, Content, Inner)) {
            continue;
        }
        // Spec MUST: the decrypted payload's sender must match the event's sender.
        if(Util::JsonGetString(Inner, "sender") != Sender) {
            continue;
        }
        if(!Inner.contains("content") || !Inner["content"].is_object()) {
            continue;
        }
        const std::string InnerType = Util::JsonGetString(Inner, "type");
        const nlohmann::json &InnerContent = Inner["content"];
        if(InnerType == "m.room_key") {
            if(m_Megolm->ImportRoomKey(InnerContent, SenderKey, false) && m_Backup != nullptr) {
                m_Backup->NoteNewSession(Util::JsonGetString(InnerContent, "room_id"), Util::JsonGetString(InnerContent, "session_id"));
            }
        } else if(InnerType == "m.forwarded_room_key") {
            if(m_Megolm->ImportRoomKey(InnerContent, SenderKey, true) && m_Backup != nullptr) {
                m_Backup->NoteNewSession(Util::JsonGetString(InnerContent, "room_id"), Util::JsonGetString(InnerContent, "session_id"));
            }
        } else if(InnerType.rfind("m.key.verification.", 0) == 0) {
            const nlohmann::json Rebuilt = {{"type", InnerType}, {"sender", Sender}, {"content", InnerContent}};
            m_Verification->HandleToDevice(Rebuilt);
        } else if(InnerType == "m.room_key.request") {
            // Some clients olm-encrypt their key requests; serve those the same way.
            HandleKeyRequest(Sender, InnerContent);
        }
    }

    if(!ChangedDeviceLists.empty()) {
        m_Devices->MarkUsersDirty(ChangedDeviceLists);
        m_Devices->QueryKeys(m_Api, {}); // lazy refresh; failures keep the dirty flags
    }

    m_LastOneTimeKeyCount = OneTimeKeyCount;
    if(m_Account->IsValid()) {
        m_Account->m_PublishedKeyCount = OneTimeKeyCount;
    }
    if(OneTimeKeyCount < MinimumOneTimeKeysDefault) {
        EnsureKeysUploaded(MinimumOneTimeKeysDefault); // best effort; swallows failures
    }
    if(m_Backup != nullptr && m_Backup->HasKey()) {
        m_Backup->PushDirtySessions(); // throttled inside; uploads only sessions new since the last push
    }
}

void E2eeManager::OnRoomMembershipChanged(const std::string &RoomId, const std::vector<std::string> &MemberUserIds) {
    m_Devices->MarkUsersDirty(MemberUserIds);
    // A departed member may hold the current key: force a reshare and rotate.
    m_SharedSessionId.erase(RoomId);
    m_SharedDevices.erase(RoomId);
    EnsureKeysUploaded(); // rotation needs the identity for the self-imported inbound copy
    m_Megolm->RotateIfNeeded(RoomId, Util::NowMs());
}

void E2eeManager::HandleKeyRequest(const std::string &SenderUserId, const nlohmann::json &RequestContent) {
    if(!RequestContent.is_object() || SenderUserId != m_UserId) {
        return; // only our own user's devices get keys from us, without verification (see header)
    }
    if(Util::JsonGetString(RequestContent, "action") != "request") {
        return; // cancellations need no handling: responses are sent immediately, not queued
    }
    if(!RequestContent.contains("body") || !RequestContent["body"].is_object()) {
        return;
    }
    const nlohmann::json &Body = RequestContent["body"];
    if(Util::JsonGetString(Body, "algorithm") != MegolmAlgorithm) {
        return;
    }
    const std::string RoomId = Util::JsonGetString(Body, "room_id");
    const std::string SessionId = Util::JsonGetString(Body, "session_id");
    const std::string RequestingDeviceId = Util::JsonGetString(RequestContent, "requesting_device_id");
    if(RoomId.empty() || SessionId.empty() || RequestingDeviceId.empty() || RequestingDeviceId == m_DeviceId) {
        return;
    }

    const nlohmann::json RoomKey = m_Megolm->BuildRoomKeyMessage(RoomId);
    if(!RoomKey.is_object() || RoomKey.empty() || Util::JsonGetString(RoomKey, "session_id") != SessionId) {
        return; // we cannot serve: the session was rotated away or never created here
    }

    DeviceIdentity Target;
    bool Found = false;
    for(const DeviceIdentity &Device : m_Devices->GetDevices(m_UserId)) {
        if(Device.DeviceId == RequestingDeviceId) {
            Target = Device;
            Found = true;
            break;
        }
    }
    if(!Found || m_Account == nullptr || !m_Account->IsValid()) {
        return;
    }

    // m.forwarded_room_key = the m.room_key fields plus the sender binding; we created
    // this session, so the sender_key is ours and the forwarding chain stays empty.
    const DeviceIdentity &Ours = m_Account->GetIdentity();
    nlohmann::json Forwarded = RoomKey;
    Forwarded["sender_key"] = Ours.Curve25519Key;
    Forwarded["sender_claimed_ed25519_key"] = Ours.Ed25519Key;
    Forwarded["forwarding_curve25519_key_chain"] = nlohmann::json::array();

    if(!m_Devices->EnsureOlmSessions(m_Api, *m_Account, *m_Sessions, {Target})) {
        return; // best effort: the requester repeats its request later
    }
    nlohmann::json Content;
    if(!m_Sessions->EncryptToDevice(Target, BuildOlmPayload("m.forwarded_room_key", Forwarded, Target), Content)) {
        return;
    }
    nlohmann::json Messages = nlohmann::json::object();
    Messages[m_UserId][Target.DeviceId] = Content;
    m_Api.SendToDevice("m.room.encrypted", Messages, MakeTransactionId()); // best effort
}

void E2eeManager::RequestMissingKey(const std::string &RoomId, const std::string &SenderKey, const std::string &SessionId,
                                    const std::string &RequestId) {
    if(RoomId.empty() || SessionId.empty() || m_UserId.empty() || m_DeviceId.empty()) {
        return;
    }
    const std::int64_t Now = Util::NowMs();
    const auto It = m_KeyRequestTime.find(SessionId);
    if(It != m_KeyRequestTime.end() && Now - It->second < KeyRequestCooldownMs) {
        return; // already asked about this session within the last 5 minutes
    }

    nlohmann::json Body;
    Body["algorithm"] = MegolmAlgorithm;
    Body["room_id"] = RoomId;
    Body["session_id"] = SessionId;
    Body["sender_key"] = SenderKey; // deprecated, still included on outgoing requests (spec)

    nlohmann::json Content;
    Content["action"] = "request";
    Content["body"] = Body;
    Content["request_id"] = RequestId;
    Content["requesting_device_id"] = m_DeviceId;

    nlohmann::json Messages = nlohmann::json::object();
    std::set<std::string> Slots;
    auto AddTarget = [&](const DeviceIdentity &Device) {
        if(Device.UserId.empty() || Device.DeviceId.empty()) {
            return;
        }
        if(Slots.insert(DeviceSlot(Device)).second) {
            Messages[Device.UserId][Device.DeviceId] = Content;
        }
    };
    // Our own devices may hold the session, and so may the device that sent the event.
    for(const DeviceIdentity &Device : m_Devices->GetDevices(m_UserId)) {
        AddTarget(Device);
    }
    for(const DeviceIdentity &Device : m_Devices->FindDevicesByCurveKey(SenderKey)) {
        AddTarget(Device);
    }
    if(Messages.empty()) {
        return; // nobody to ask: no known devices yet
    }

    // Spec: m.room_key.request is sent unencrypted to-device.
    if(!m_Api.SendToDevice("m.room_key.request", Messages, MakeTransactionId()).Ok) {
        return; // failed: do not start the cooldown, the next failure retries
    }
    m_KeyRequestTime[SessionId] = Now;
    // Keep the timestamp map small: drop entries whose cooldown has expired.
    for(auto Prune = m_KeyRequestTime.begin(); Prune != m_KeyRequestTime.end();) {
        if(Now - Prune->second >= KeyRequestCooldownMs) {
            Prune = m_KeyRequestTime.erase(Prune);
        } else {
            ++Prune;
        }
    }
}

DeviceIdentity E2eeManager::GetDeviceIdentity() const {
    if(m_Account != nullptr && m_Account->IsValid()) {
        return m_Account->GetIdentity();
    }
    DeviceIdentity Identity;
    Identity.UserId = m_UserId;
    Identity.DeviceId = m_DeviceId;
    return Identity;
}

} // namespace NxMatrix::E2ee