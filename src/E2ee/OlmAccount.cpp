// olm_account wrapper: identity keys, self-signatures, one-time keys and pickles.
#include "NxMatrix/E2ee/OlmAccount.hpp"

#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/Util/Json.hpp"

#include <olm/olm.h>
#include <vector>

namespace NxMatrix::E2ee {

namespace {

// libolm 3.2.16 reports every failure as olm_error() (== (size_t)-1) with the
// reason in last_error; there are no separate out-of-bounds/short-buffer codes.
bool Failed(std::size_t Result) {
    return Result == olm_error();
}

// Reads a JSON blob olm wrote into a sized buffer (identity keys, one-time keys).
nlohmann::json ParseOlmJson(const char *Data, std::size_t Length) {
    return nlohmann::json::parse(Data, Data + Length, nullptr, false);
}

} // namespace

OlmAccount::OlmAccount() {
    m_Buffer.resize(olm_account_size());
    m_Handle = olm_account(m_Buffer.data());
}

OlmAccount::~OlmAccount() {
    if(m_Handle != nullptr) {
        olm_clear_account(static_cast<::OlmAccount *>(m_Handle));
        m_Handle = nullptr;
    }
    m_Valid = false;
}

bool OlmAccount::CreateNew(const std::string &UserId, const std::string &DeviceId) {
    if(m_Handle == nullptr || UserId.empty() || DeviceId.empty()) {
        return false;
    }

    const std::size_t RandomLength = olm_create_account_random_length(static_cast<::OlmAccount *>(m_Handle));
    ByteArray Random(RandomLength);
    Crypto::FillRandom(Random.data(), Random.size());
    if(Failed(olm_create_account(static_cast<::OlmAccount *>(m_Handle), Random.data(), Random.size()))) {
        m_Valid = false;
        return false;
    }

    // Identity keys come back as {"curve25519":"...","ed25519":"..."} (unpadded base64).
    const std::size_t KeysLength = olm_account_identity_keys_length(static_cast<::OlmAccount *>(m_Handle));
    std::vector<char> KeysBuffer(KeysLength + 1, '\0');
    const std::size_t Written = olm_account_identity_keys(static_cast<::OlmAccount *>(m_Handle), KeysBuffer.data(), KeysBuffer.size());
    if(Failed(Written)) {
        m_Valid = false;
        return false;
    }
    const nlohmann::json Keys = ParseOlmJson(KeysBuffer.data(), Written);
    if(!Keys.is_object()) {
        m_Valid = false;
        return false;
    }

    m_Identity.UserId = UserId;
    m_Identity.DeviceId = DeviceId;
    m_Identity.Curve25519Key = Util::JsonGetString(Keys, "curve25519");
    m_Identity.Ed25519Key = Util::JsonGetString(Keys, "ed25519");
    m_Valid = !m_Identity.Curve25519Key.empty() && !m_Identity.Ed25519Key.empty();
    m_PublishedKeyCount = 0;
    return m_Valid;
}

const DeviceIdentity &OlmAccount::GetIdentity() const {
    return m_Identity;
}

bool OlmAccount::IsValid() const {
    return m_Valid;
}

nlohmann::json OlmAccount::ExportState() const {
    nlohmann::json State = nlohmann::json::object();
    if(!m_Valid || m_Handle == nullptr) {
        return State; // empty object: E2eeManager records "no account yet"
    }

    auto *Handle = static_cast<::OlmAccount *>(m_Handle);
    const std::size_t PickledLength = olm_pickle_account_length(Handle);
    std::vector<std::uint8_t> Pickled(PickledLength);
    // olm_pickle_account already emits base64 (unpadded, standard alphabet) of the
    // encrypted pickle, so the string is stored as-is.
    const std::size_t Written = olm_pickle_account(Handle, m_PickleKey.data(), m_PickleKey.size(), Pickled.data(), Pickled.size());
    if(Failed(Written)) {
        return nlohmann::json::object(); // no pickle to hand back: recorded as "no account"
    }

    State["version"] = 1;
    State["pickle"] = std::string(Pickled.begin(), Pickled.begin() + static_cast<std::ptrdiff_t>(Written));
    // The pickle covers the key material only; our user/device binding lives here.
    State["user_id"] = m_Identity.UserId;
    State["device_id"] = m_Identity.DeviceId;
    return State;
}

bool OlmAccount::ImportState(const nlohmann::json &State, const Crypto::KeyBytes &PickleKey) {
    m_PickleKey = PickleKey;
    m_Valid = false;
    if(!State.is_object() || State.empty()) {
        return false; // fresh install: stay invalid so EnsureKeysUploaded creates a new account
    }
    if(!State.contains("version") || !State["version"].is_number_integer() || State["version"].get<std::int64_t>() != 1 ||
       !State.contains("pickle") || !State["pickle"].is_string()) {
        return false;
    }

    // olm_unpickle_account destroys the input buffer, so hand it a copy.
    std::string Pickle = State["pickle"].get<std::string>();
    if(Pickle.empty()) {
        return false;
    }
    ByteArray Copy(Pickle.begin(), Pickle.end());
    auto *Handle = static_cast<::OlmAccount *>(m_Handle);
    if(Failed(olm_unpickle_account(Handle, PickleKey.data(), PickleKey.size(), Copy.data(), Copy.size()))) {
        return false; // wrong key or corrupted pickle
    }

    const std::size_t KeysLength = olm_account_identity_keys_length(Handle);
    std::vector<char> KeysBuffer(KeysLength + 1, '\0');
    const std::size_t Written = olm_account_identity_keys(Handle, KeysBuffer.data(), KeysBuffer.size());
    if(Failed(Written)) {
        return false;
    }
    const nlohmann::json Keys = ParseOlmJson(KeysBuffer.data(), Written);
    if(!Keys.is_object()) {
        return false;
    }

    m_Identity.UserId = Util::JsonGetString(State, "user_id");
    m_Identity.DeviceId = Util::JsonGetString(State, "device_id");
    m_Identity.Curve25519Key = Util::JsonGetString(Keys, "curve25519");
    m_Identity.Ed25519Key = Util::JsonGetString(Keys, "ed25519");
    m_Valid =
        !m_Identity.UserId.empty() && !m_Identity.DeviceId.empty() && !m_Identity.Curve25519Key.empty() && !m_Identity.Ed25519Key.empty();
    return m_Valid;
}

nlohmann::json OlmAccount::BuildDeviceKeys() const {
    nlohmann::json DeviceKeys;
    if(!m_Valid) {
        return DeviceKeys;
    }

    DeviceKeys["user_id"] = m_Identity.UserId;
    DeviceKeys["device_id"] = m_Identity.DeviceId;
    DeviceKeys["algorithms"] = nlohmann::json::array({"m.olm.v1.curve25519-aes-sha2", "m.megolm.v1.aes-sha2"});
    nlohmann::json Keys;
    Keys["ed25519:" + m_Identity.DeviceId] = m_Identity.Ed25519Key;
    Keys["curve25519:" + m_Identity.DeviceId] = m_Identity.Curve25519Key;
    DeviceKeys["keys"] = Keys;

    // The signature covers the object without its "signatures" property (Signing JSON).
    const std::string Signature = Sign(DeviceKeys);
    if(Signature.empty()) {
        return nlohmann::json();
    }
    DeviceKeys["signatures"] = nlohmann::json{{m_Identity.UserId, {{"ed25519:" + m_Identity.DeviceId, Signature}}}};
    return DeviceKeys;
}

std::map<std::string, nlohmann::json> OlmAccount::GenerateSignedOneTimeKeys(std::size_t Count) {
    std::map<std::string, nlohmann::json> Out;
    if(!m_Valid || Count == 0) {
        return Out;
    }
    auto *Handle = static_cast<::OlmAccount *>(m_Handle);

    const std::size_t RandomLength = olm_account_generate_one_time_keys_random_length(Handle, Count);
    ByteArray Random(RandomLength);
    Crypto::FillRandom(Random.data(), Random.size());
    if(Failed(olm_account_generate_one_time_keys(Handle, Count, Random.data(), Random.size()))) {
        return Out;
    }

    const std::size_t KeysLength = olm_account_one_time_keys_length(Handle);
    std::vector<char> KeysBuffer(KeysLength + 1, '\0');
    const std::size_t Written = olm_account_one_time_keys(Handle, KeysBuffer.data(), KeysBuffer.size());
    if(Failed(Written)) {
        return Out;
    }
    // {"curve25519": {"<key id>": "<base64 key>"}} — unpublished keys only.
    const nlohmann::json Raw = ParseOlmJson(KeysBuffer.data(), Written);
    if(!Raw.is_object() || !Raw.contains("curve25519") || !Raw["curve25519"].is_object()) {
        return Out;
    }

    for(auto It = Raw["curve25519"].begin(); It != Raw["curve25519"].end(); ++It) {
        if(!It.value().is_string()) {
            continue;
        }
        nlohmann::json Signed;
        Signed["key"] = It.value();
        const std::string Signature = Sign(Signed);
        if(Signature.empty()) {
            continue;
        }
        Signed["signatures"] = nlohmann::json{{m_Identity.UserId, {{"ed25519:" + m_Identity.DeviceId, Signature}}}};
        Out["signed_curve25519:" + It.key()] = Signed;
    }
    if(Out.empty()) {
        return Out;
    }

    // From here the keys are upload-bound; the server count is refreshed by
    // E2eeManager from the /keys/upload response.
    olm_account_mark_keys_as_published(Handle);
    return Out;
}

std::size_t OlmAccount::GetOneTimeKeyCount() const {
    return m_PublishedKeyCount;
}

std::string OlmAccount::Sign(const nlohmann::json &Object) const {
    if(!m_Valid || !Object.is_object()) {
        return {};
    }
    auto *Handle = static_cast<::OlmAccount *>(m_Handle);
    const std::string Message = Util::CanonicalJson(Object);

    const std::size_t SignatureLength = olm_account_signature_length(Handle);
    std::vector<std::uint8_t> Signature(SignatureLength);
    // olm_account_sign writes unpadded standard-base64 — exactly what Matrix signatures need.
    const std::size_t Written = olm_account_sign(Handle, Message.data(), Message.size(), Signature.data(), Signature.size());
    if(Failed(Written)) {
        return {};
    }
    return std::string(Signature.begin(), Signature.begin() + static_cast<std::ptrdiff_t>(Written));
}

} // namespace NxMatrix::E2ee