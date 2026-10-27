// Megolm group sessions: one outbound session per room, inbound per (sender key, session id).
#include "NxMatrix/E2ee/MegolmManager.hpp"

#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <olm/olm.h>
#include <vector>

namespace NxMatrix::E2ee {

namespace {

constexpr const char *MegolmAlgorithm = "m.megolm.v1.aes-sha2";

bool Failed(std::size_t Result) {
    return Result == olm_error(); // libolm's only error sentinel
}

::OlmOutboundGroupSession *AsOutbound(void *Handle) {
    return static_cast<::OlmOutboundGroupSession *>(Handle);
}

::OlmInboundGroupSession *AsInbound(void *Handle) {
    return static_cast<::OlmInboundGroupSession *>(Handle);
}

std::string InboundSessionId(::OlmInboundGroupSession *Session) {
    const std::size_t Length = olm_inbound_group_session_id_length(Session);
    std::vector<std::uint8_t> Buffer(Length);
    const std::size_t Written = olm_inbound_group_session_id(Session, Buffer.data(), Buffer.size());
    if(Failed(Written)) {
        return {};
    }
    return std::string(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Written));
}

std::string OutboundSessionId(::OlmOutboundGroupSession *Session) {
    const std::size_t Length = olm_outbound_group_session_id_length(Session);
    std::vector<std::uint8_t> Buffer(Length);
    const std::size_t Written = olm_outbound_group_session_id(Session, Buffer.data(), Buffer.size());
    if(Failed(Written)) {
        return {};
    }
    return std::string(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Written));
}

} // namespace

nlohmann::json MegolmManager::ExportState() const {
    nlohmann::json State;
    State["outbound"] = nlohmann::json::object();
    State["inbound"] = nlohmann::json::object();
    State["inbound_room_ids"] = m_InboundRoomId;

    for(const auto &[RoomId, Entry] : m_Outbound) {
        if(Entry.Handle == nullptr) {
            continue;
        }
        auto *Session = AsOutbound(Entry.Handle);
        const std::size_t PickledLength = olm_pickle_outbound_group_session_length(Session);
        std::vector<std::uint8_t> Pickled(PickledLength);
        const std::size_t Written =
            olm_pickle_outbound_group_session(Session, m_PickleKey.data(), m_PickleKey.size(), Pickled.data(), Pickled.size());
        if(Failed(Written)) {
            continue;
        }
        nlohmann::json Item;
        // Rotation bookkeeping must survive restarts or the session never rotates.
        Item["pickle"] = std::string(Pickled.begin(), Pickled.begin() + static_cast<std::ptrdiff_t>(Written));
        Item["created_ms"] = Entry.CreatedMs;
        Item["message_count"] = static_cast<std::int64_t>(Entry.MessageCount);
        State["outbound"][RoomId] = Item;
    }

    for(const auto &[Key, Entry] : m_Inbound) {
        if(Entry.Handle == nullptr) {
            continue;
        }
        auto *Session = AsInbound(Entry.Handle);
        // Skip anything that is not a "<sender curve key>|<session id>" index (defensive).
        const std::size_t Split = Key.find('|');
        if(Split == std::string::npos || Split == 0 || Split + 1 >= Key.size() || InboundSessionId(Session).empty()) {
            continue;
        }
        const std::size_t PickledLength = olm_pickle_inbound_group_session_length(Session);
        std::vector<std::uint8_t> Pickled(PickledLength);
        const std::size_t Written =
            olm_pickle_inbound_group_session(Session, m_PickleKey.data(), m_PickleKey.size(), Pickled.data(), Pickled.size());
        if(Failed(Written)) {
            continue;
        }
        State["inbound"][Key] = std::string(Pickled.begin(), Pickled.begin() + static_cast<std::ptrdiff_t>(Written));
    }
    return State;
}

bool MegolmManager::ImportState(const nlohmann::json &State, const Crypto::KeyBytes &PickleKey) {
    m_PickleKey = PickleKey;
    if(!State.is_object()) {
        return false;
    }

    for(auto &[RoomId, Entry] : m_Outbound) {
        if(Entry.Handle != nullptr) {
            olm_clear_outbound_group_session(AsOutbound(Entry.Handle));
        }
    }
    m_Outbound.clear();
    for(auto &[Key, Entry] : m_Inbound) {
        if(Entry.Handle != nullptr) {
            olm_clear_inbound_group_session(AsInbound(Entry.Handle));
        }
    }
    m_Inbound.clear();
    m_InboundRoomId.clear();
    if(State.contains("inbound_room_ids") && State["inbound_room_ids"].is_object()) {
        for(auto It = State["inbound_room_ids"].begin(); It != State["inbound_room_ids"].end(); ++It) {
            if(It.value().is_string()) {
                m_InboundRoomId[It.key()] = It.value().get<std::string>();
            }
        }
    }

    if(State.contains("outbound") && State["outbound"].is_object()) {
        const nlohmann::json &Outbound = State["outbound"];
        for(auto It = Outbound.begin(); It != Outbound.end(); ++It) {
            if(!It.value().is_object() || !It.value().contains("pickle") || !It.value()["pickle"].is_string()) {
                continue;
            }
            OutboundEntry Entry;
            Entry.Buffer.resize(olm_outbound_group_session_size());
            Entry.Handle = olm_outbound_group_session(Entry.Buffer.data());
            std::string Pickle = It.value()["pickle"].get<std::string>();
            ByteArray Copy(Pickle.begin(), Pickle.end());
            if(Failed(olm_unpickle_outbound_group_session(AsOutbound(Entry.Handle), PickleKey.data(), PickleKey.size(), Copy.data(),
                                                          Copy.size()))) {
                olm_clear_outbound_group_session(AsOutbound(Entry.Handle));
                continue;
            }
            Entry.CreatedMs = Util::JsonGetInt(It.value(), "created_ms", Util::NowMs());
            const std::int64_t Count = Util::JsonGetInt(It.value(), "message_count", 0);
            Entry.MessageCount = Count > 0 ? static_cast<std::size_t>(Count) : 0;
            m_Outbound[It.key()] = std::move(Entry);
        }
    }

    if(State.contains("inbound") && State["inbound"].is_object()) {
        const nlohmann::json &Inbound = State["inbound"];
        for(auto It = Inbound.begin(); It != Inbound.end(); ++It) {
            if(!It.value().is_string()) {
                continue;
            }
            const std::size_t Split = It.key().find('|');
            if(Split == std::string::npos || Split == 0 || Split + 1 >= It.key().size()) {
                continue;
            }
            const std::string Owner = It.key().substr(0, Split);
            const std::string SessionId = It.key().substr(Split + 1);

            std::string Pickle = It.value().get<std::string>();
            if(Pickle.empty()) {
                continue;
            }
            InboundEntry Entry;
            Entry.Buffer.resize(olm_inbound_group_session_size());
            Entry.Handle = olm_inbound_group_session(Entry.Buffer.data());
            ByteArray Copy(Pickle.begin(), Pickle.end());
            if(Failed(olm_unpickle_inbound_group_session(AsInbound(Entry.Handle), PickleKey.data(), PickleKey.size(), Copy.data(),
                                                         Copy.size()))) {
                olm_clear_inbound_group_session(AsInbound(Entry.Handle));
                continue;
            }
            if(InboundSessionId(AsInbound(Entry.Handle)) != SessionId) {
                olm_clear_inbound_group_session(AsInbound(Entry.Handle));
                continue;
            }
            m_Inbound[Owner + "|" + SessionId] = std::move(Entry);
        }
    }
    return true;
}

bool MegolmManager::EnsureOutboundSession(const std::string &RoomId) {
    if(RoomId.empty()) {
        return false;
    }
    if(m_Outbound.find(RoomId) != m_Outbound.end()) {
        return true;
    }

    OutboundEntry Entry;
    Entry.Buffer.resize(olm_outbound_group_session_size());
    Entry.Handle = olm_outbound_group_session(Entry.Buffer.data());
    auto *Session = AsOutbound(Entry.Handle);

    const std::size_t RandomLength = olm_init_outbound_group_session_random_length(Session);
    ByteArray Random(RandomLength);
    Crypto::FillRandom(Random.data(), Random.size());
    if(Failed(olm_init_outbound_group_session(Session, Random.data(), Random.size()))) {
        olm_clear_outbound_group_session(Session);
        return false;
    }
    Entry.CreatedMs = Util::NowMs();
    Entry.MessageCount = 0;
    m_Outbound[RoomId] = std::move(Entry);

    // The sending device never receives its own to-device room key (E2eeManager skips
    // itself when sharing), so keep an inbound copy of our own session — otherwise we
    // could not decrypt our own events after a restart.
    if(m_OurIdentity.Curve25519Key.empty()) {
        return true;
    }
    auto *OurSession = AsOutbound(m_Outbound[RoomId].Handle);
    const std::size_t KeyLength = olm_outbound_group_session_key_length(OurSession);
    std::vector<std::uint8_t> KeyBuffer(KeyLength);
    const std::size_t KeyWritten = olm_outbound_group_session_key(OurSession, KeyBuffer.data(), KeyBuffer.size());
    const std::string SessionId = OutboundSessionId(OurSession);
    if(Failed(KeyWritten) || SessionId.empty()) {
        DropOutboundSession(RoomId);
        return false;
    }

    InboundEntry Inbound;
    Inbound.Buffer.resize(olm_inbound_group_session_size());
    Inbound.Handle = olm_inbound_group_session(Inbound.Buffer.data());
    std::vector<std::uint8_t> KeyCopy(KeyBuffer.begin(), KeyBuffer.begin() + static_cast<std::ptrdiff_t>(KeyWritten));
    // The outbound key is the signed v2 session-key format: olm_init_inbound_group_session
    // verifies its signature, same path as a received m.room_key.
    if(Failed(olm_init_inbound_group_session(AsInbound(Inbound.Handle), KeyCopy.data(), KeyCopy.size())) ||
       InboundSessionId(AsInbound(Inbound.Handle)) != SessionId) {
        olm_clear_inbound_group_session(AsInbound(Inbound.Handle));
        DropOutboundSession(RoomId);
        return false;
    }
    const std::string Key = m_OurIdentity.Curve25519Key + "|" + SessionId;
    auto Existing = m_Inbound.find(Key);
    if(Existing != m_Inbound.end()) {
        olm_clear_inbound_group_session(AsInbound(Existing->second.Handle));
        Existing->second = std::move(Inbound);
    } else {
        m_Inbound[Key] = std::move(Inbound);
    }
    m_InboundRoomId[SessionId] = RoomId;
    return true;
}

bool MegolmManager::RotateIfNeeded(const std::string &RoomId, std::int64_t NowMs) {
    auto It = m_Outbound.find(RoomId);
    if(It != m_Outbound.end()) {
        const bool OverMessages = It->second.MessageCount >= MaxMessages;
        const bool OverAge = NowMs - It->second.CreatedMs >= MaxAgeMs;
        if(!OverMessages && !OverAge) {
            return true; // session still within rotation limits
        }
        // Only the outbound session rotates; inbound copies of the old session stay so
        // already-sent messages remain decryptable.
        DropOutboundSession(RoomId);
    }
    return EnsureOutboundSession(RoomId);
}

void MegolmManager::DropOutboundSession(const std::string &RoomId) {
    auto It = m_Outbound.find(RoomId);
    if(It == m_Outbound.end()) {
        return;
    }
    if(It->second.Handle != nullptr) {
        olm_clear_outbound_group_session(AsOutbound(It->second.Handle));
    }
    m_Outbound.erase(It);
}

bool MegolmManager::EncryptRoomEvent(const std::string &RoomId, const std::string &EventType, const nlohmann::json &PlainContent,
                                     nlohmann::json &OutEncryptedContent) {
    if(m_OurIdentity.Curve25519Key.empty() || m_OurIdentity.DeviceId.empty()) {
        return false; // identity not established yet; E2eeManager uploads keys first
    }
    if(!EnsureOutboundSession(RoomId)) {
        return false;
    }

    OutboundEntry &Entry = m_Outbound[RoomId];
    auto *Session = AsOutbound(Entry.Handle);

    // Spec payload: {type, content, room_id} — room_id binds the event to this room.
    nlohmann::json Payload;
    Payload["type"] = EventType;
    Payload["content"] = PlainContent;
    Payload["room_id"] = RoomId;
    const std::string Plain = Util::CanonicalJson(Payload);

    const std::size_t MessageLength = olm_group_encrypt_message_length(Session, Plain.size());
    if(Failed(MessageLength)) {
        return false;
    }
    std::vector<std::uint8_t> Message(MessageLength);
    const std::size_t Written =
        olm_group_encrypt(Session, reinterpret_cast<const std::uint8_t *>(Plain.data()), Plain.size(), Message.data(), Message.size());
    if(Failed(Written)) {
        return false;
    }
    Entry.MessageCount++;

    const std::string SessionId = OutboundSessionId(Session);
    if(SessionId.empty()) {
        return false;
    }

    OutEncryptedContent = nlohmann::json::object();
    OutEncryptedContent["algorithm"] = MegolmAlgorithm;
    OutEncryptedContent["sender_key"] = m_OurIdentity.Curve25519Key;
    OutEncryptedContent["device_id"] = m_OurIdentity.DeviceId;
    OutEncryptedContent["session_id"] = SessionId;
    // Spec: "As with all event relationships, the m.relates_to property must
    // be sent in the unencrypted (cleartext) part of the event." Servers read
    // relations (replies, edits, reactions, threads) from the outer content.
    if(PlainContent.is_object()) {
        const auto Rel = PlainContent.find("m.relates_to");
        if(Rel != PlainContent.end()) {
            OutEncryptedContent["m.relates_to"] = *Rel;
        }
    }
    // olm_group_encrypt already outputs base64.
    OutEncryptedContent["ciphertext"] = std::string(Message.begin(), Message.begin() + static_cast<std::ptrdiff_t>(Written));
    return true;
}

nlohmann::json MegolmManager::BuildRoomKeyMessage(const std::string &RoomId) const {
    nlohmann::json RoomKey;
    auto It = m_Outbound.find(RoomId);
    if(It == m_Outbound.end() || It->second.Handle == nullptr) {
        return RoomKey; // empty object: no outbound session for this room
    }
    auto *Session = AsOutbound(It->second.Handle);

    const std::string SessionId = OutboundSessionId(Session);
    // Read at share time: the key encodes the CURRENT ratchet state and message index.
    const std::size_t KeyLength = olm_outbound_group_session_key_length(Session);
    std::vector<std::uint8_t> KeyBuffer(KeyLength);
    const std::size_t KeyWritten = olm_outbound_group_session_key(Session, KeyBuffer.data(), KeyBuffer.size());
    if(SessionId.empty() || Failed(KeyWritten)) {
        return nlohmann::json();
    }

    RoomKey["algorithm"] = MegolmAlgorithm;
    RoomKey["room_id"] = RoomId;
    RoomKey["session_id"] = SessionId;
    RoomKey["session_key"] = std::string(KeyBuffer.begin(), KeyBuffer.begin() + static_cast<std::ptrdiff_t>(KeyWritten));
    RoomKey["chain_index"] = static_cast<std::int64_t>(olm_outbound_group_session_message_index(Session));
    return RoomKey;
}

bool MegolmManager::ImportRoomKey(const nlohmann::json &RoomKeyContent, const std::string &SenderCurveKey, bool Forwarded) {
    if(!RoomKeyContent.is_object()) {
        return false;
    }
    const std::string Algorithm = Util::JsonGetString(RoomKeyContent, "algorithm");
    const std::string SessionId = Util::JsonGetString(RoomKeyContent, "session_id");
    const std::string SessionKey = Util::JsonGetString(RoomKeyContent, "session_key");
    if(Algorithm != MegolmAlgorithm || SessionId.empty() || SessionKey.empty()) {
        return false;
    }

    // A forwarded key names the ORIGINAL session creator in sender_key; a plain m.room_key
    // arrives from the creator itself, so the olm wrapper's sender key is authoritative.
    const std::string ClaimedSender = Util::JsonGetString(RoomKeyContent, "sender_key");
    const std::string Owner = (Forwarded && !ClaimedSender.empty()) ? ClaimedSender : SenderCurveKey;
    if(Owner.empty()) {
        return false;
    }

    const std::string Key = Owner + "|" + SessionId;
    if(m_Inbound.find(Key) != m_Inbound.end()) {
        return true; // already have this session: keep the first copy (spec forbids replacing with later keys)
    }

    InboundEntry Entry;
    Entry.Buffer.resize(olm_inbound_group_session_size());
    Entry.Handle = olm_inbound_group_session(Entry.Buffer.data());

    // Two session-key formats exist in the wild: the signed v2 key from olm_outbound_...
    // _key (m.room_key, and our own forwarded serves) and the v1 export format
    // (re-forwarded keys). Try the signed format first, then the export format. olm's
    // docs warn the key input may be overwritten, so each attempt gets a fresh copy.
    std::vector<std::uint8_t> Copy(SessionKey.begin(), SessionKey.end());
    bool Ok = !Failed(olm_init_inbound_group_session(AsInbound(Entry.Handle), Copy.data(), Copy.size()));
    if(!Ok) {
        Copy.assign(SessionKey.begin(), SessionKey.end());
        Ok = !Failed(olm_import_inbound_group_session(AsInbound(Entry.Handle), Copy.data(), Copy.size()));
    }
    if(!Ok) {
        olm_clear_inbound_group_session(AsInbound(Entry.Handle));
        return false;
    }
    if(InboundSessionId(AsInbound(Entry.Handle)) != SessionId) {
        // The content claims a session id the key does not produce: refuse to mix them.
        olm_clear_inbound_group_session(AsInbound(Entry.Handle));
        return false;
    }
    m_Inbound[Key] = std::move(Entry);
    const std::string RoomId = Util::JsonGetString(RoomKeyContent, "room_id");
    if(!RoomId.empty()) {
        m_InboundRoomId[SessionId] = RoomId;
    }
    return true;
}

bool MegolmManager::CanDecrypt(const std::string &SenderCurveKey, const std::string &SessionId) const {
    if(SenderCurveKey.empty() || SessionId.empty()) {
        return false;
    }
    auto It = m_Inbound.find(SenderCurveKey + "|" + SessionId);
    return It != m_Inbound.end() && It->second.Handle != nullptr;
}

bool MegolmManager::DecryptRoomEvent(const std::string &SenderCurveKey, const std::string &SessionId, const std::string &Ciphertext,
                                     nlohmann::json &OutPlainContent, std::string &OutEventType) {
    auto It = m_Inbound.find(SenderCurveKey + "|" + SessionId);
    if(It == m_Inbound.end() || It->second.Handle == nullptr || Ciphertext.empty()) {
        return false;
    }
    auto *Session = AsInbound(It->second.Handle);

    // olm_group_decrypt destroys its input: work on a copy of the base64 ciphertext.
    std::vector<std::uint8_t> Message(Ciphertext.begin(), Ciphertext.end());
    const std::size_t MaxPlain = olm_group_decrypt_max_plaintext_length(Session, Message.data(), Message.size());
    if(Failed(MaxPlain)) {
        return false;
    }
    std::vector<std::uint8_t> MessageCopy(Ciphertext.begin(), Ciphertext.end());
    std::vector<std::uint8_t> Plain(MaxPlain);
    std::uint32_t MessageIndex = 0;
    const std::size_t Written =
        olm_group_decrypt(Session, MessageCopy.data(), MessageCopy.size(), Plain.data(), Plain.size(), &MessageIndex);
    if(Failed(Written)) {
        return false;
    }

    const nlohmann::json Payload =
        nlohmann::json::parse(Plain.begin(), Plain.begin() + static_cast<std::ptrdiff_t>(Written), nullptr, false);
    if(!Payload.is_object()) {
        return false;
    }
    const std::string EventType = Util::JsonGetString(Payload, "type");
    if(EventType.empty() || !Payload.contains("content") || !Payload["content"].is_object()) {
        return false;
    }
    OutEventType = EventType;
    OutPlainContent = Payload["content"];
    return true;
}

std::vector<MegolmManager::ExportedSession> MegolmManager::ExportAllSessions() const {
    std::vector<ExportedSession> Out;
    for(const auto &[Key, Entry] : m_Inbound) {
        if(Entry.Handle == nullptr) {
            continue;
        }
        const std::size_t Split = Key.find('|');
        if(Split == std::string::npos || Split == 0 || Split + 1 >= Key.size()) {
            continue; // not a "<sender curve key>|<session id>" index (defensive)
        }
        const std::string Owner = Key.substr(0, Split);
        const std::string SessionId = Key.substr(Split + 1);
        auto *Session = AsInbound(Entry.Handle);
        if(InboundSessionId(Session) != SessionId) {
            continue;
        }
        const auto RoomIt = m_InboundRoomId.find(SessionId);
        if(RoomIt == m_InboundRoomId.end() || RoomIt->second.empty()) {
            continue; // no room id: the backup format cannot store it without one
        }
        // Export at the first index we can decrypt: max fidelity for the backup.
        const std::uint32_t FirstIndex = olm_inbound_group_session_first_known_index(Session);
        const std::size_t KeyLength = olm_export_inbound_group_session_length(Session);
        if(KeyLength == 0) {
            continue;
        }
        std::vector<std::uint8_t> KeyBuffer(KeyLength);
        const std::size_t Written = olm_export_inbound_group_session(Session, KeyBuffer.data(), KeyBuffer.size(), FirstIndex);
        if(Failed(Written)) {
            continue; // corrupt session: skip, never throw
        }
        ExportedSession Item;
        Item.RoomId = RoomIt->second;
        Item.SessionId = SessionId;
        Item.SessionKey.assign(KeyBuffer.begin(), KeyBuffer.begin() + static_cast<std::ptrdiff_t>(Written));
        Item.SenderCurveKey = Owner;
        Item.FirstMessageIndex = static_cast<int>(FirstIndex);
        Item.ForwardedCount = 0; // forward depth is not tracked locally
        Out.push_back(std::move(Item));
    }
    return Out;
}

std::size_t MegolmManager::GetInboundSessionCount() const {
    std::size_t Count = 0;
    for(const auto &[Key, Entry] : m_Inbound) {
        if(Entry.Handle != nullptr) {
            ++Count;
        }
    }
    return Count;
}

} // namespace NxMatrix::E2ee