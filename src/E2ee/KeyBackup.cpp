// Server-side key backup (4S): encrypts exported megolm session data with a
// curve25519 key (libolm olm/pk.h) and stores it at /room_keys/*.
// Wire shapes follow spec v1.19 "Backing up room keys"; the displayed
// recovery key follows the spec "Cryptographic key representation" exactly
// as matrix-js-sdk (Element) implements it (0x8B 0x01 header + XOR parity,
// base58, groups of 4).
#include "NxMatrix/E2ee/KeyBackup.hpp"

#include "NxMatrix/Crypto/KeyDerivation.hpp"
#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/E2ee/MegolmManager.hpp"
#include "NxMatrix/E2ee/OlmAccount.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Util/Base64.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <olm/olm.h>
#include <olm/pk.h>
#include <set>

namespace NxMatrix::E2ee {

namespace {

constexpr const char *BackupAlgorithm = "m.megolm_backup.v1.curve25519-aes-sha2";
constexpr const char *MegolmAlgorithm = "m.megolm.v1.aes-sha2";
// Element's recovery-key derivation cost (matrix-js-sdk RECOVERY_KEY_DERIVATION_ITERATIONS).
constexpr std::uint32_t PassphraseIterations = 500000;
constexpr std::size_t PassphraseSaltBytes = 16;
constexpr std::int64_t PushIntervalMs = 30000; // sync-loop throttle between dirty pushes
const char *Base58Alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool Failed(std::size_t Result) {
    return Result == olm_error(); // libolm's only error sentinel
}

std::string Base58Encode(const std::uint8_t *Data, std::size_t Size) {
    std::size_t Zeros = 0;
    while(Zeros < Size && Data[Zeros] == 0) {
        ++Zeros;
    }
    std::vector<unsigned> Num(Data + Zeros, Data + Size);
    std::string Out;
    while(!Num.empty()) {
        unsigned Carry = 0;
        for(unsigned &Digit : Num) {
            const unsigned Current = (Carry << 8) + Digit;
            Digit = Current / 58;
            Carry = Current % 58;
        }
        Out.push_back(Base58Alphabet[Carry]);
        while(!Num.empty() && Num.front() == 0) {
            Num.erase(Num.begin());
        }
    }
    Out.append(Zeros, '1');
    std::reverse(Out.begin(), Out.end());
    return Out;
}

bool Base58Decode(const std::string &Text, ByteArray &Out) {
    std::size_t Zeros = 0;
    while(Zeros < Text.size() && Text[Zeros] == '1') {
        ++Zeros;
    }
    std::vector<unsigned> Num;
    for(std::size_t I = Zeros; I < Text.size(); ++I) {
        const char *Pos = std::find(Base58Alphabet, Base58Alphabet + 58, Text[I]);
        if(Pos == Base58Alphabet + 58) {
            return false;
        }
        unsigned Carry = static_cast<unsigned>(Pos - Base58Alphabet);
        for(unsigned &Digit : Num) {
            Carry += Digit * 58;
            Digit = Carry & 0xFF;
            Carry >>= 8;
        }
        while(Carry != 0) {
            Num.push_back(Carry & 0xFF);
            Carry >>= 8;
        }
    }
    while(!Num.empty() && Num.back() == 0) {
        Num.pop_back();
    }
    Out.assign(Zeros, 0);
    for(auto It = Num.rbegin(); It != Num.rend(); ++It) {
        Out.push_back(static_cast<std::uint8_t>(*It));
    }
    return true;
}

// Spec appendices "Cryptographic key representation": two header bytes
// 0x8B 0x01, the raw key, XOR parity of all bytes, base58, spaces every 4th char.
std::string EncodeRecoveryKey(const ByteArray &Key) {
    ByteArray Buf;
    Buf.reserve(Key.size() + 3);
    Buf.push_back(0x8B);
    Buf.push_back(0x01);
    Buf.insert(Buf.end(), Key.begin(), Key.end());
    std::uint8_t Parity = 0;
    for(std::uint8_t Byte : Buf) {
        Parity ^= Byte;
    }
    Buf.push_back(Parity);
    const std::string Raw = Base58Encode(Buf.data(), Buf.size());
    std::string Out;
    for(std::size_t I = 0; I < Raw.size(); I += 4) {
        if(!Out.empty()) {
            Out.push_back(' ');
        }
        Out.append(Raw, I, std::min<std::size_t>(4, Raw.size() - I));
    }
    return Out;
}

bool DecodeRecoveryKey(const std::string &Text, ByteArray &OutKey) {
    std::string Compact;
    for(char Ch : Text) {
        if(!std::isspace(static_cast<unsigned char>(Ch))) {
            Compact.push_back(Ch);
        }
    }
    ByteArray Buf;
    if(!Base58Decode(Compact, Buf)) {
        return false;
    }
    if(Buf.size() != 35 || Buf[0] != 0x8B || Buf[1] != 0x01) {
        return false;
    }
    std::uint8_t Parity = 0;
    for(std::uint8_t Byte : Buf) {
        Parity ^= Byte;
    }
    if(Parity != 0) {
        return false; // matrix-js-sdk rejects this with "Incorrect parity"
    }
    OutKey.assign(Buf.begin() + 2, Buf.begin() + 34);
    return OutKey.size() == 32;
}

std::string StripKeyPadding(const std::string &Text) {
    std::string Out;
    for(char Ch : Text) {
        if(Ch != '=' && !std::isspace(static_cast<unsigned char>(Ch))) {
            Out.push_back(Ch);
        }
    }
    return Out;
}

// Raw 32-byte private key -> unpadded base64 curve25519 public key (libolm).
bool PublicKeyFromPrivate(const ByteArray &PrivateKey, std::string &OutPublicKey) {
    if(PrivateKey.size() != olm_pk_private_key_length()) {
        return false;
    }
    std::vector<std::uint8_t> Buffer(olm_pk_decryption_size());
    ::OlmPkDecryption *Decryption = olm_pk_decryption(Buffer.data());
    const std::size_t KeyLength = olm_pk_key_length();
    std::vector<std::uint8_t> Public(KeyLength + 1, 0);
    const bool Ok = !Failed(olm_pk_key_from_private(Decryption, Public.data(), Public.size(), PrivateKey.data(), PrivateKey.size()));
    olm_clear_pk_decryption(Decryption);
    if(!Ok) {
        return false;
    }
    OutPublicKey.assign(Public.begin(), Public.begin() + static_cast<std::ptrdiff_t>(KeyLength));
    return true;
}

// spec 13.11 "Uploading session data": olm_pk_encrypt output stored as the
// algorithm-dependent session_data {ciphertext, ephemeral, mac}.
bool EncryptSessionData(const std::string &PlainText, const std::string &PublicKey, nlohmann::json &OutSessionData) {
    if(PublicKey.size() < olm_pk_key_length()) {
        return false;
    }
    std::vector<std::uint8_t> Buffer(olm_pk_encryption_size());
    ::OlmPkEncryption *Encryption = olm_pk_encryption(Buffer.data());
    if(Failed(olm_pk_encryption_set_recipient_key(Encryption, PublicKey.data(), PublicKey.size()))) {
        return false;
    }
    const std::size_t CiphertextLength = olm_pk_ciphertext_length(Encryption, PlainText.size());
    const std::size_t MacLength = olm_pk_mac_length(Encryption);
    const std::size_t EphemeralLength = olm_pk_key_length();
    const std::size_t RandomLength = olm_pk_encrypt_random_length(Encryption);
    std::vector<std::uint8_t> Ciphertext(CiphertextLength);
    std::vector<std::uint8_t> Mac(MacLength);
    std::vector<std::uint8_t> Ephemeral(EphemeralLength);
    std::vector<std::uint8_t> Random(RandomLength);
    Crypto::FillRandom(Random.data(), Random.size());
    const bool Ok = !Failed(olm_pk_encrypt(Encryption, PlainText.data(), PlainText.size(), Ciphertext.data(), Ciphertext.size(), Mac.data(),
                                           Mac.size(), Ephemeral.data(), Ephemeral.size(), Random.data(), Random.size()));
    olm_clear_pk_encryption(Encryption);
    if(!Ok) {
        return false;
    }
    OutSessionData = nlohmann::json{{"ciphertext", std::string(Ciphertext.begin(), Ciphertext.end())},
                                    {"ephemeral", std::string(Ephemeral.begin(), Ephemeral.end())},
                                    {"mac", std::string(Mac.begin(), Mac.end())}};
    return true;
}

bool DecryptSessionData(const nlohmann::json &SessionData, const ByteArray &PrivateKey, std::string &OutPlainText) {
    if(!SessionData.is_object() || PrivateKey.size() != olm_pk_private_key_length()) {
        return false;
    }
    const std::string CiphertextText = Util::JsonGetString(SessionData, "ciphertext");
    const std::string EphemeralText = Util::JsonGetString(SessionData, "ephemeral");
    const std::string MacText = Util::JsonGetString(SessionData, "mac");
    if(CiphertextText.empty() || EphemeralText.empty() || MacText.empty()) {
        return false;
    }
    std::vector<std::uint8_t> Buffer(olm_pk_decryption_size());
    ::OlmPkDecryption *Decryption = olm_pk_decryption(Buffer.data());
    std::vector<std::uint8_t> Public(olm_pk_key_length() + 1, 0);
    if(Failed(olm_pk_key_from_private(Decryption, Public.data(), Public.size(), PrivateKey.data(), PrivateKey.size()))) {
        olm_clear_pk_decryption(Decryption);
        return false;
    }
    // olm_pk_decrypt destroys its input buffers: each gets a fresh copy.
    std::vector<std::uint8_t> Ciphertext(CiphertextText.begin(), CiphertextText.end());
    std::vector<std::uint8_t> Ephemeral(EphemeralText.begin(), EphemeralText.end());
    std::vector<std::uint8_t> Mac(MacText.begin(), MacText.end());
    const std::size_t MaxPlain = olm_pk_max_plaintext_length(Decryption, Ciphertext.size());
    if(Failed(MaxPlain)) {
        olm_clear_pk_decryption(Decryption);
        return false;
    }
    std::vector<std::uint8_t> Plain(MaxPlain);
    const std::size_t Written = olm_pk_decrypt(Decryption, Ephemeral.data(), Ephemeral.size(), Mac.data(), Mac.size(), Ciphertext.data(),
                                               Ciphertext.size(), Plain.data(), Plain.size());
    olm_clear_pk_decryption(Decryption);
    if(Failed(Written)) {
        return false;
    }
    OutPlainText.assign(Plain.begin(), Plain.begin() + static_cast<std::ptrdiff_t>(Written));
    return true;
}

std::string StateString(const nlohmann::json &State, const char *Key) {
    return State.is_object() && State.contains(Key) && State[Key].is_string() ? State[Key].get<std::string>() : std::string();
}

} // namespace

KeyBackup::KeyBackup(Matrix::Client &Api, MegolmManager &Megolm) : m_Api(Api), m_Megolm(Megolm) {}

nlohmann::json KeyBackup::ExportState() const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    return m_State.is_object() ? m_State : nlohmann::json::object();
}

void KeyBackup::ImportState(const nlohmann::json &State) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_State = State.is_object() ? State : nlohmann::json::object();
}

bool KeyBackup::HasKey() const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    return !StateString(m_State, "version").empty() && !StateString(m_State, "private_key").empty();
}

bool KeyBackup::CreateBackup(const std::string &Passphrase, std::string &OutRecoveryKey) {
    // Key material: 32 random bytes as the private key (libolm's documented
    // keygen path), or PBKDF2-derived from the passphrase.
    ByteArray PrivateKey = Crypto::RandomBytes(olm_pk_private_key_length());
    std::string Salt;
    std::uint32_t Iterations = 0;
    if(!Passphrase.empty()) {
        const ByteArray SaltRaw = Crypto::RandomBytes(PassphraseSaltBytes);
        Salt = Util::Base64Encode(SaltRaw);
        Iterations = PassphraseIterations;
        Crypto::SaltBytes SaltArray{};
        std::copy(SaltRaw.begin(), SaltRaw.end(), SaltArray.begin());
        const Crypto::KeyBytes Derived = Crypto::KeyDerivation::DeriveKey(Passphrase, SaltArray, Iterations);
        PrivateKey.assign(Derived.begin(), Derived.end());
    }

    std::string PublicKey;
    if(!PublicKeyFromPrivate(PrivateKey, PublicKey)) {
        return false;
    }

    nlohmann::json AuthData = {{"public_key", PublicKey}};
    if(!Salt.empty()) {
        AuthData["private_key_salt"] = Salt;
        AuthData["private_key_iterations"] = static_cast<std::int64_t>(Iterations);
    }
    // Sign the auth_data without its signatures slot (signed JSON), so other
    // clients can trust the backup through our device identity.
    if(m_Account != nullptr) {
        const std::string Signature = m_Account->Sign(AuthData);
        const DeviceIdentity &Identity = m_Account->GetIdentity();
        if(!Signature.empty() && !Identity.UserId.empty() && !Identity.DeviceId.empty()) {
            AuthData["signatures"] = nlohmann::json{{Identity.UserId, {{"ed25519:" + Identity.DeviceId, Signature}}}};
        }
    }

    const auto Version = m_Api.CreateRoomKeysVersion({{"algorithm", BackupAlgorithm}, {"auth_data", AuthData}});
    if(!Version.Ok || Version.Value.empty()) {
        return false;
    }

    OutRecoveryKey = EncodeRecoveryKey(PrivateKey);
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        if(!m_State.is_object()) {
            m_State = nlohmann::json::object();
        }
        m_State["version"] = Version.Value;
        m_State["algorithm"] = BackupAlgorithm;
        m_State["private_key"] = Util::Base64Encode(PrivateKey);
        m_State["salt"] = Salt;
        m_State["iterations"] = static_cast<std::int64_t>(Iterations);
        m_State["recovery_key"] = OutRecoveryKey;
        m_State["uploaded_count"] = static_cast<std::int64_t>(0);
        m_State["last_push_ms"] = static_cast<std::int64_t>(0);
        m_DirtySessions.clear(); // fresh backup starts empty
    }
    return true;
}

bool KeyBackup::ActivateWithRecoveryKey(const std::string &RecoveryKey) {
    ByteArray PrivateKey;
    if(!DecodeRecoveryKey(RecoveryKey, PrivateKey)) {
        return false;
    }
    std::string DerivedPublic;
    if(!PublicKeyFromPrivate(PrivateKey, DerivedPublic)) {
        return false;
    }

    const auto Latest = m_Api.GetRoomKeysLatestVersion();
    if(!Latest.Ok) {
        return false;
    }
    const std::string Algorithm = Util::JsonGetString(Latest.Value, "algorithm");
    if(!Algorithm.empty() && Algorithm != BackupAlgorithm) {
        return false; // unknown backup algorithm: refuse
    }
    const nlohmann::json AuthData =
        Latest.Value.contains("auth_data") && Latest.Value["auth_data"].is_object() ? Latest.Value["auth_data"] : nlohmann::json::object();
    const std::string ServerPublic = Util::JsonGetString(AuthData, "public_key");
    if(ServerPublic.empty() || StripKeyPadding(ServerPublic) != StripKeyPadding(DerivedPublic)) {
        return false; // the key does not unlock this backup
    }
    const std::string Version = Util::JsonGetString(Latest.Value, "version");
    if(Version.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        if(!m_State.is_object()) {
            m_State = nlohmann::json::object();
        }
        m_State["version"] = Version;
        m_State["algorithm"] = Algorithm.empty() ? std::string(BackupAlgorithm) : Algorithm;
        m_State["private_key"] = Util::Base64Encode(PrivateKey);
        m_State["salt"] = Util::JsonGetString(AuthData, "private_key_salt");
        m_State["iterations"] = Util::JsonGetInt(AuthData, "private_key_iterations", 0);
        m_State["recovery_key"] = EncodeRecoveryKey(PrivateKey);
        m_State["uploaded_count"] = Util::JsonGetInt(Latest.Value, "count", 0);
        // Keep the dirty set: new local sessions still need uploading here.
    }
    return true;
}

KeyBackup::Status KeyBackup::GetStatus() {
    Status Out;
    const auto Local = m_Megolm.ExportAllSessions();
    Out.LocalSessions = static_cast<int>(Local.size());

    nlohmann::json StateCopy;
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        StateCopy = m_State;
    }
    const std::string StateVersion = StateString(StateCopy, "version");
    const bool StateHasKey = !StateString(StateCopy, "private_key").empty();
    const std::string RecoveryKey = StateString(StateCopy, "recovery_key");

    const auto Latest = m_Api.GetRoomKeysLatestVersion();
    if(!Latest.Ok) {
        if(Latest.Error.StatusCode == 404) {
            return Out; // no backup configured on this account
        }
        // Transient failure: report what we know locally instead of lying about setup.
        Out.Exists = !StateVersion.empty();
        Out.Version = StateVersion;
        Out.Algorithm = StateString(StateCopy, "algorithm");
        Out.UploadedSessions = static_cast<int>(Util::JsonGetInt(StateCopy, "uploaded_count", 0));
        Out.KeyAvailable = StateHasKey;
        if(!RecoveryKey.empty()) {
            Out.KeyFingerprint = RecoveryKey.substr(0, 8);
        }
        return Out;
    }

    Out.Exists = true;
    Out.Version = Util::JsonGetString(Latest.Value, "version");
    Out.Algorithm = Util::JsonGetString(Latest.Value, "algorithm");
    Out.UploadedSessions = static_cast<int>(Util::JsonGetInt(Latest.Value, "count", 0));
    const bool SameVersion = !Out.Version.empty() && Out.Version == StateVersion;
    Out.KeyAvailable = StateHasKey && SameVersion;
    if(Out.KeyAvailable && !RecoveryKey.empty()) {
        Out.KeyFingerprint = RecoveryKey.substr(0, 8);
    }
    return Out;
}

bool KeyBackup::UploadAllSessions() {
    nlohmann::json StateCopy;
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        StateCopy = m_State;
    }
    const std::string Version = StateString(StateCopy, "version");
    const ByteArray PrivateKey = Util::Base64Decode(StateString(StateCopy, "private_key"));
    if(Version.empty() || PrivateKey.size() != olm_pk_private_key_length()) {
        return false; // no usable key: cannot encrypt sessions for the backup
    }
    std::string PublicKey;
    if(!PublicKeyFromPrivate(PrivateKey, PublicKey)) {
        return false;
    }

    // What the server already has: only the missing sessions get uploaded.
    const auto Existing = m_Api.GetRoomKeysSessions(Version, "", "");
    if(!Existing.Ok) {
        return false;
    }
    std::set<std::string> Uploaded;
    int ExistingCount = 0;
    if(Existing.Value.contains("rooms") && Existing.Value["rooms"].is_object()) {
        const nlohmann::json &Rooms = Existing.Value["rooms"];
        for(auto RoomIt = Rooms.begin(); RoomIt != Rooms.end(); ++RoomIt) {
            if(!RoomIt.value().is_object() || !RoomIt.value().contains("sessions") || !RoomIt.value()["sessions"].is_object()) {
                continue;
            }
            const nlohmann::json &Sessions = RoomIt.value()["sessions"];
            for(auto SessionIt = Sessions.begin(); SessionIt != Sessions.end(); ++SessionIt) {
                Uploaded.insert(RoomIt.key() + "|" + SessionIt.key());
                ++ExistingCount;
            }
        }
    }

    const auto Local = m_Megolm.ExportAllSessions();
    const DeviceIdentity Identity = m_Account != nullptr ? m_Account->GetIdentity() : DeviceIdentity{};
    nlohmann::json Body = nlohmann::json::object();
    int Added = 0;
    for(const auto &Session : Local) {
        if(Session.RoomId.empty() || Session.SessionId.empty() || Session.SessionKey.empty() || Session.SenderCurveKey.empty()) {
            continue; // the spec requires room_id and sender_key in the payload
        }
        if(Uploaded.count(Session.RoomId + "|" + Session.SessionId) != 0) {
            continue; // already on the server
        }

        // BackedUpSessionData (spec "Backing up room keys"), then olm_pk_encrypt.
        nlohmann::json PlainData;
        PlainData["algorithm"] = MegolmAlgorithm;
        PlainData["forwarding_curve25519_key_chain"] = nlohmann::json::array();
        PlainData["session_key"] = Session.SessionKey;
        PlainData["sender_key"] = Session.SenderCurveKey;
        nlohmann::json ClaimedKeys = nlohmann::json::object();
        if(Session.SenderCurveKey == Identity.Curve25519Key && !Identity.Ed25519Key.empty()) {
            ClaimedKeys["ed25519"] = Identity.Ed25519Key; // our own sessions: sender fully known
        }
        PlainData["sender_claimed_keys"] = ClaimedKeys;

        nlohmann::json SessionData;
        if(!EncryptSessionData(PlainData.dump(), PublicKey, SessionData)) {
            continue; // olm failure: skip this session, never throw
        }
        nlohmann::json Entry;
        Entry["first_message_index"] = Session.FirstMessageIndex;
        Entry["forwarded_count"] = Session.ForwardedCount;
        Entry["is_verified"] = false; // device-level trust of the sender is not tracked per session
        Entry["session_data"] = SessionData;
        Body["rooms"][Session.RoomId]["sessions"][Session.SessionId] = Entry;
        ++Added;
    }

    if(Added > 0) {
        const auto Put = m_Api.UploadRoomKeys(Version, Body);
        if(!Put.Ok) {
            return false;
        }
    }

    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_State["uploaded_count"] = static_cast<std::int64_t>(ExistingCount + Added);
    // Everything local was either already on the server or just uploaded;
    // unexportable sessions would fail again forever, so drop them too.
    m_DirtySessions.clear();
    return true;
}

bool KeyBackup::RestoreAllSessions() {
    nlohmann::json StateCopy;
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        StateCopy = m_State;
    }
    const std::string Version = StateString(StateCopy, "version");
    const ByteArray PrivateKey = Util::Base64Decode(StateString(StateCopy, "private_key"));
    if(Version.empty() || PrivateKey.size() != olm_pk_private_key_length()) {
        return false;
    }

    const auto All = m_Api.GetRoomKeysSessions(Version, "", "");
    if(!All.Ok) {
        return false;
    }

    int Seen = 0;
    int Imported = 0;
    if(All.Value.contains("rooms") && All.Value["rooms"].is_object()) {
        const nlohmann::json &Rooms = All.Value["rooms"];
        for(auto RoomIt = Rooms.begin(); RoomIt != Rooms.end(); ++RoomIt) {
            if(!RoomIt.value().is_object() || !RoomIt.value().contains("sessions") || !RoomIt.value()["sessions"].is_object()) {
                continue;
            }
            const std::string RoomId = RoomIt.key();
            const nlohmann::json &Sessions = RoomIt.value()["sessions"];
            for(auto SessionIt = Sessions.begin(); SessionIt != Sessions.end(); ++SessionIt) {
                if(!SessionIt.value().is_object()) {
                    continue;
                }
                ++Seen;
                const std::string SessionId = SessionIt.key();
                const nlohmann::json &Data = SessionIt.value();
                if(!Data.contains("session_data") || !Data["session_data"].is_object()) {
                    continue;
                }
                std::string PlainText;
                if(!DecryptSessionData(Data["session_data"], PrivateKey, PlainText)) {
                    continue; // not decryptable with this key: skip, never throw
                }
                const nlohmann::json Plain = nlohmann::json::parse(PlainText, nullptr, false);
                if(!Plain.is_object()) {
                    continue;
                }
                const std::string Algorithm = Util::JsonGetString(Plain, "algorithm");
                const std::string SenderKey = Util::JsonGetString(Plain, "sender_key");
                const std::string SessionKey = Util::JsonGetString(Plain, "session_key");
                if(Algorithm != MegolmAlgorithm || SenderKey.empty() || SessionKey.empty()) {
                    continue; // incomplete BackedUpSessionData: unusable
                }
                bool Forwarded = false;
                if(Plain.contains("forwarding_curve25519_key_chain") && Plain["forwarding_curve25519_key_chain"].is_array()) {
                    Forwarded = !Plain["forwarding_curve25519_key_chain"].empty();
                }
                nlohmann::json Content;
                Content["algorithm"] = Algorithm;
                Content["room_id"] = RoomId;
                Content["session_id"] = SessionId;
                Content["session_key"] = SessionKey;
                Content["sender_key"] = SenderKey;
                if(m_Megolm.ImportRoomKey(Content, SenderKey, Forwarded)) {
                    ++Imported; // counts sessions already present: they are usable either way
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        m_State["uploaded_count"] = static_cast<std::int64_t>(Seen);
    }
    return Seen == 0 || Imported > 0; // keys present but none usable = failed restore
}

void KeyBackup::NoteNewSession(const std::string &RoomId, const std::string &SessionId) {
    if(RoomId.empty() || SessionId.empty()) {
        return;
    }
    const std::string Entry = RoomId + "|" + SessionId;
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(std::find(m_DirtySessions.begin(), m_DirtySessions.end(), Entry) == m_DirtySessions.end()) {
        m_DirtySessions.push_back(Entry);
    }
}

void KeyBackup::PushDirtySessions() {
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        if(StateString(m_State, "version").empty() || StateString(m_State, "private_key").empty()) {
            return; // no backup key: nothing to push
        }
        if(m_DirtySessions.empty() || m_Pushing) {
            return;
        }
        const auto Now = std::chrono::steady_clock::now();
        if(m_LastPush != std::chrono::steady_clock::time_point{} && Now - m_LastPush < std::chrono::milliseconds(PushIntervalMs)) {
            return; // inside the throttle window
        }
        m_Pushing = true;
        m_LastPush = Now;
        m_State["last_push_ms"] = static_cast<std::int64_t>(Util::NowMs());
    }
    (void)UploadAllSessions(); // UploadAllSessions already uploads only what the server lacks
    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_Pushing = false;
}

} // namespace NxMatrix::E2ee