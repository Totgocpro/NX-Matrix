// Olm pairwise sessions: outbound creation, to-device encrypt/decrypt and pickles.
#include "NxMatrix/E2ee/OlmSessionManager.hpp"

#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/E2ee/OlmAccount.hpp"
#include "NxMatrix/Util/Json.hpp"

#include <olm/olm.h>
#include <vector>

namespace NxMatrix::E2ee {

namespace {

constexpr const char *OlmAlgorithm = "m.olm.v1.curve25519-aes-sha2";

bool Failed(std::size_t Result) {
    return Result == olm_error(); // libolm's only error sentinel
}

::OlmSession *AsSession(void *Handle) {
    return static_cast<::OlmSession *>(Handle);
}

std::string SessionIdOf(::OlmSession *Session) {
    const std::size_t Length = olm_session_id_length(Session);
    std::vector<std::uint8_t> Buffer(Length);
    const std::size_t Written = olm_session_id(Session, Buffer.data(), Buffer.size());
    if(Failed(Written)) {
        return {};
    }
    return std::string(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Written));
}

// olm_decrypt_max_plaintext_length destroys its input, and olm_decrypt destroys its
// own input again — two independent copies of the base64 body are required.
bool TryDecryptMessage(::OlmSession *Session, std::size_t MessageType, const std::string &Body, nlohmann::json &OutPlain) {
    if(Session == nullptr) {
        return false;
    }
    std::vector<std::uint8_t> Probe(Body.begin(), Body.end());
    const std::size_t MaxPlain = olm_decrypt_max_plaintext_length(Session, MessageType, Probe.data(), Probe.size());
    if(Failed(MaxPlain)) {
        return false;
    }
    std::vector<std::uint8_t> MessageCopy(Body.begin(), Body.end());
    std::vector<std::uint8_t> Plain(MaxPlain);
    const std::size_t Written = olm_decrypt(Session, MessageType, MessageCopy.data(), MessageCopy.size(), Plain.data(), Plain.size());
    if(Failed(Written)) {
        return false;
    }

    OutPlain = nlohmann::json::parse(Plain.begin(), Plain.begin() + static_cast<std::ptrdiff_t>(Written), nullptr, false);
    return OutPlain.is_object();
}

// Spec MUST after decrypting: the payload must name us as recipient. (Checks against
// the sender's device keys need /keys/query verification, which is out of scope.)
bool ValidateOlmPayload(const nlohmann::json &Payload, const DeviceIdentity &Ours) {
    if(!Payload.is_object()) {
        return false;
    }
    if(Util::JsonGetString(Payload, "recipient") != Ours.UserId) {
        return false;
    }
    if(!Payload.contains("recipient_keys") || !Payload["recipient_keys"].is_object()) {
        return false;
    }
    return Util::JsonGetString(Payload["recipient_keys"], "ed25519") == Ours.Ed25519Key;
}

} // namespace

nlohmann::json OlmSessionManager::ExportState() const {
    nlohmann::json State;
    State["sessions"] = nlohmann::json::object();
    for(const auto &[Target, Entry] : m_Sessions) {
        if(Entry.Handle == nullptr) {
            continue;
        }
        auto *Session = AsSession(Entry.Handle);
        const std::string SessionId = SessionIdOf(Session);
        if(SessionId.empty()) {
            continue;
        }
        const std::size_t PickledLength = olm_pickle_session_length(Session);
        std::vector<std::uint8_t> Pickled(PickledLength);
        const std::size_t Written = olm_pickle_session(Session, m_PickleKey.data(), m_PickleKey.size(), Pickled.data(), Pickled.size());
        if(Failed(Written)) {
            continue;
        }
        State["sessions"][Target + "|" + SessionId] = std::string(Pickled.begin(), Pickled.begin() + static_cast<std::ptrdiff_t>(Written));
    }
    return State;
}

bool OlmSessionManager::ImportState(const nlohmann::json &State, const Crypto::KeyBytes &PickleKey) {
    m_PickleKey = PickleKey;
    if(!State.is_object()) {
        return false;
    }

    // Old sessions are dropped wholesale: the state fully describes our session set.
    for(auto &[Target, Entry] : m_Sessions) {
        if(Entry.Handle != nullptr) {
            olm_clear_session(AsSession(Entry.Handle));
        }
    }
    m_Sessions.clear();

    if(!State.contains("sessions") || !State["sessions"].is_object()) {
        return true; // tolerated empty section
    }
    const nlohmann::json &Sessions = State["sessions"];
    for(auto It = Sessions.begin(); It != Sessions.end(); ++It) {
        if(!It.value().is_string()) {
            continue;
        }
        // Key: "<sender curve25519 key>|<olm session id>" — both are base64, so '|' is unambiguous.
        const std::string &Key = It.key();
        const std::size_t Split = Key.find('|');
        if(Split == std::string::npos || Split == 0 || Split + 1 >= Key.size()) {
            continue;
        }
        const std::string SenderKey = Key.substr(0, Split);
        const std::string SessionId = Key.substr(Split + 1);

        std::string Pickle = It.value().get<std::string>();
        if(Pickle.empty()) {
            continue;
        }
        SessionEntry Entry;
        Entry.Buffer.resize(olm_session_size());
        Entry.Handle = olm_session(Entry.Buffer.data());
        // olm_unpickle_session destroys its input: hand it a copy.
        ByteArray Copy(Pickle.begin(), Pickle.end());
        if(Failed(olm_unpickle_session(AsSession(Entry.Handle), PickleKey.data(), PickleKey.size(), Copy.data(), Copy.size()))) {
            continue; // wrong pickle key on a single entry: skip, tolerate the rest
        }
        // Guard against a corrupted index: the loaded session must be the one the key named.
        if(SessionIdOf(AsSession(Entry.Handle)) != SessionId) {
            olm_clear_session(AsSession(Entry.Handle));
            continue;
        }
        m_Sessions[SenderKey] = std::move(Entry);
    }
    return true;
}

bool OlmSessionManager::HasSessionFor(const std::string &TargetCurveKey) const {
    auto It = m_Sessions.find(TargetCurveKey);
    return It != m_Sessions.end() && It->second.Handle != nullptr;
}

bool OlmSessionManager::CreateOutboundSession(const DeviceIdentity &TargetDevice, const std::string &OneTimeKeyId,
                                              const std::string &SignedOneTimeKey) {
    (void)OneTimeKeyId; // only the raw key reaches libolm; the id is bookkeeping for callers
    if(m_Account == nullptr || !m_Account->IsValid() || TargetDevice.Curve25519Key.empty() || SignedOneTimeKey.empty()) {
        return false;
    }

    SessionEntry Entry;
    auto Existing = m_Sessions.find(TargetDevice.Curve25519Key);
    if(Existing != m_Sessions.end()) {
        Entry = std::move(Existing->second); // reuse the buffer, wipe the old session in place
    }
    Entry.Buffer.resize(olm_session_size());
    Entry.Handle = olm_session(Entry.Buffer.data());

    auto *Session = AsSession(Entry.Handle);
    auto *Account = static_cast<::OlmAccount *>(m_Account->m_Handle);
    const std::size_t RandomLength = olm_create_outbound_session_random_length(Session);
    ByteArray Random(RandomLength);
    Crypto::FillRandom(Random.data(), Random.size());

    if(Failed(olm_create_outbound_session(Session, Account, TargetDevice.Curve25519Key.data(), TargetDevice.Curve25519Key.size(),
                                          SignedOneTimeKey.data(), SignedOneTimeKey.size(), Random.data(), Random.size()))) {
        olm_clear_session(Session);
        if(Existing != m_Sessions.end()) {
            m_Sessions.erase(Existing);
        }
        return false;
    }
    m_Sessions[TargetDevice.Curve25519Key] = std::move(Entry);
    return true;
}

bool OlmSessionManager::EncryptToDevice(const DeviceIdentity &TargetDevice, const nlohmann::json &PlainJson, nlohmann::json &OutContent) {
    if(m_OurIdentity.Curve25519Key.empty() || m_OurIdentity.DeviceId.empty() || TargetDevice.Curve25519Key.empty()) {
        return false;
    }
    auto It = m_Sessions.find(TargetDevice.Curve25519Key);
    if(It == m_Sessions.end() || It->second.Handle == nullptr) {
        return false; // no session yet: DeviceTracker.EnsureOlmSessions is responsible for creating one
    }

    auto *Session = AsSession(It->second.Handle);
    const std::string Plain = Util::CanonicalJson(PlainJson);

    const std::size_t MessageLength = olm_encrypt_message_length(Session, Plain.size());
    if(Failed(MessageLength)) {
        return false;
    }
    std::vector<std::uint8_t> Message(MessageLength);
    const std::size_t RandomLength = olm_encrypt_random_length(Session);
    ByteArray Random(RandomLength);
    Crypto::FillRandom(Random.data(), Random.size());

    const std::size_t Written =
        olm_encrypt(Session, Plain.data(), Plain.size(), Random.data(), Random.size(), Message.data(), Message.size());
    if(Failed(Written)) {
        // An unusable session cannot be recovered from; drop it so the next
        // EnsureOlmSessions call replaces it with a fresh outbound session.
        olm_clear_session(Session);
        m_Sessions.erase(It);
        return false;
    }
    const std::size_t MessageType = olm_encrypt_message_type(Session);
    if(MessageType > OLM_MESSAGE_TYPE_MESSAGE) {
        return false;
    }

    nlohmann::json CiphertextEntry;
    CiphertextEntry["type"] = static_cast<int>(MessageType); // 0 = pre-key message until they reply
    CiphertextEntry["body"] = std::string(Message.begin(), Message.begin() + static_cast<std::ptrdiff_t>(Written));

    OutContent = nlohmann::json::object();
    OutContent["algorithm"] = OlmAlgorithm;
    OutContent["sender_key"] = m_OurIdentity.Curve25519Key;
    // sender_device follows the assignment's payload; device_id is the spec's field for
    // the sending device on m.room.encrypted — receivers read either, extra keys are ignored.
    OutContent["sender_device"] = m_OurIdentity.DeviceId;
    OutContent["device_id"] = m_OurIdentity.DeviceId;
    OutContent["ciphertext"] = nlohmann::json{{TargetDevice.Curve25519Key, CiphertextEntry}};
    return true;
}

bool OlmSessionManager::DecryptFromDevice(const std::string &SenderCurveKey, const nlohmann::json &EncryptedContent,
                                          nlohmann::json &OutPlain) {
    if(m_Account == nullptr || !m_Account->IsValid() || SenderCurveKey.empty() || m_OurIdentity.Curve25519Key.empty()) {
        return false;
    }
    if(!EncryptedContent.is_object() || !EncryptedContent.contains("ciphertext") || !EncryptedContent["ciphertext"].is_object()) {
        return false;
    }
    const nlohmann::json &CipherMap = EncryptedContent["ciphertext"];
    if(!CipherMap.contains(m_OurIdentity.Curve25519Key) || !CipherMap[m_OurIdentity.Curve25519Key].is_object()) {
        return false; // not encrypted for this device
    }
    const nlohmann::json &EntryJson = CipherMap[m_OurIdentity.Curve25519Key];
    const std::int64_t MessageType = Util::JsonGetInt(EntryJson, "type", -1);
    const std::string Body = Util::JsonGetString(EntryJson, "body");
    if((MessageType != 0 && MessageType != 1) || Body.empty()) {
        return false;
    }

    auto *Account = static_cast<::OlmAccount *>(m_Account->m_Handle);
    auto It = m_Sessions.find(SenderCurveKey);

    // Spec: type 0 first tries an existing matching session, then falls back to a new
    // inbound session; type 1 only ever uses an existing session.
    if(It != m_Sessions.end() && It->second.Handle != nullptr) {
        nlohmann::json ExistingPlain;
        if(TryDecryptMessage(AsSession(It->second.Handle), static_cast<std::size_t>(MessageType), Body, ExistingPlain)) {
            if(!ValidateOlmPayload(ExistingPlain, m_OurIdentity)) {
                return false;
            }
            OutPlain = std::move(ExistingPlain);
            return true;
        }
        if(MessageType == 1) {
            // Spec: an undecryptable message means the session is corrupt — drop it so a
            // fresh outbound session is created on the next EnsureOlmSessions call.
            olm_clear_session(AsSession(It->second.Handle));
            m_Sessions.erase(It);
            return false;
        }
        // Type 0: the pre-key message may belong to a different session — replace below.
    }

    if(MessageType != 0) {
        return false; // type 1 with no usable session is an invalid message
    }

    SessionEntry Fresh;
    Fresh.Buffer.resize(olm_session_size());
    Fresh.Handle = olm_session(Fresh.Buffer.data());
    auto *Session = AsSession(Fresh.Handle);

    // olm_create_inbound_session_from destroys the message buffer: pass a copy.
    std::vector<std::uint8_t> MessageCopy(Body.begin(), Body.end());
    if(Failed(olm_create_inbound_session_from(Session, Account, SenderCurveKey.data(), SenderCurveKey.size(), MessageCopy.data(),
                                              MessageCopy.size()))) {
        olm_clear_session(Session);
        return false;
    }

    nlohmann::json FreshPlain;
    if(!TryDecryptMessage(Session, 0, Body, FreshPlain)) {
        // Never persist a session before it successfully decrypted a message (spec).
        olm_clear_session(Session);
        return false;
    }
    if(!ValidateOlmPayload(FreshPlain, m_OurIdentity)) {
        olm_clear_session(Session);
        return false;
    }
    if(It != m_Sessions.end()) {
        olm_clear_session(AsSession(It->second.Handle));
        It->second = std::move(Fresh);
    } else {
        m_Sessions[SenderCurveKey] = std::move(Fresh);
    }
    OutPlain = std::move(FreshPlain);
    return true;
}

} // namespace NxMatrix::E2ee