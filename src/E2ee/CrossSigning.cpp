// Cross-signing: key generation, spec-shaped signing and blocking /keys/* uploads.
#include "NxMatrix/E2ee/CrossSigning.hpp"

#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/E2ee/OlmAccount.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Types.hpp"
#include "NxMatrix/Util/Base64.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/String.hpp"

#include <cstddef>
#include <string>

// Internal ed25519 primitives of libolm 3.2.16 (the tag pinned in CMakeLists): declared by
// the private header lib/ed25519/src/ed25519.h and compiled into the library by src/ed25519.c.
// ED25519_DECLSPEC expands to nothing outside Windows, so plain extern "C" declarations link
// against the shipped objects. Layout: 32-byte public key, 64-byte private key (clamped
// SHA-512 of the seed), 64-byte signature; ed25519_verify returns non-zero when valid.
extern "C" {
void ed25519_create_keypair(unsigned char *Public, unsigned char *Private, const unsigned char *Seed);
void ed25519_sign(unsigned char *Signature, const unsigned char *Message, std::size_t MessageLength, const unsigned char *Public,
                  const unsigned char *Private);
int ed25519_verify(const unsigned char *Signature, const unsigned char *Message, std::size_t MessageLength, const unsigned char *Public);
}

namespace NxMatrix::E2ee {

namespace {

constexpr std::size_t SeedLength = 32;
constexpr std::size_t PublicKeyLength = 32;
constexpr std::size_t PrivateKeyLength = 64;
constexpr std::size_t SignatureLength = 64;

// State slots double as the spec "usage" values of the uploaded key objects.
constexpr const char *NameMaster = "master";
constexpr const char *NameSelfSigning = "self_signing";
constexpr const char *NameUserSigning = "user_signing";
constexpr const char *KeyNames[] = {NameMaster, NameSelfSigning, NameUserSigning};

struct Ed25519Pair {
    ByteArray Public;
    ByteArray Private;
};

// The single "ed25519:<id>" entry of a CrossSigningKey "keys" map.
struct KeyEntry {
    std::string Id;    // key id without the algorithm prefix
    std::string Value; // unpadded base64 public key
};

// Matrix encodes keys and signatures as unpadded standard base64 (spec "Signing JSON").
std::string Base64Unpadded(const ByteArray &Data) {
    std::string Encoded = Util::Base64Encode(Data);
    while(!Encoded.empty() && Encoded.back() == '=') {
        Encoded.pop_back();
    }
    return Encoded;
}

const nlohmann::json *FindPair(const nlohmann::json &State, const char *Name) {
    if(!State.is_object()) {
        return nullptr;
    }
    const auto It = State.find(Name);
    if(It == State.end() || !It->is_object()) {
        return nullptr;
    }
    if(Util::JsonGetString(*It, "priv").empty() || Util::JsonGetString(*It, "pub").empty()) {
        return nullptr;
    }
    return &*It;
}

// Restores raw key bytes from a state entry; empty members on malformed base64.
Ed25519Pair DecodePair(const nlohmann::json &Pair) {
    Ed25519Pair Out;
    Out.Private = Util::Base64Decode(Util::JsonGetString(Pair, "priv"));
    Out.Public = Util::Base64Decode(Util::JsonGetString(Pair, "pub"));
    if(Out.Private.size() != PrivateKeyLength || Out.Public.size() != PublicKeyLength) {
        return {};
    }
    return Out;
}

void GeneratePair(nlohmann::json &State, const char *Name) {
    const ByteArray Seed = Crypto::RandomBytes(SeedLength);
    ByteArray Public(PublicKeyLength);
    ByteArray Private(PrivateKeyLength);
    ed25519_create_keypair(Public.data(), Private.data(), Seed.data());
    State[Name] = nlohmann::json{{"priv", Base64Unpadded(Private)}, {"pub", Base64Unpadded(Public)}};
}

// Signing JSON: signatures and unsigned are excluded from the signed bytes.
nlohmann::json WithoutSignatures(nlohmann::json Object) {
    if(Object.is_object()) {
        Object.erase("signatures");
        Object.erase("unsigned");
    }
    return Object;
}

std::string SignObject(const Ed25519Pair &Pair, const nlohmann::json &Object) {
    const std::string Message = Util::CanonicalJson(WithoutSignatures(Object));
    ByteArray Signature(SignatureLength);
    ed25519_sign(Signature.data(), reinterpret_cast<const unsigned char *>(Message.data()), Message.size(),
                 reinterpret_cast<const unsigned char *>(Pair.Public.data()), Pair.Private.data());
    return Base64Unpadded(Signature);
}

bool VerifySignature(const std::string &PublicKey, const std::string &Message, const std::string &Signature) {
    const ByteArray Key = Util::Base64Decode(PublicKey);
    const ByteArray Sig = Util::Base64Decode(Signature);
    if(Key.size() != PublicKeyLength || Sig.size() != SignatureLength) {
        return false;
    }
    return ed25519_verify(Sig.data(), reinterpret_cast<const unsigned char *>(Message.data()), Message.size(), Key.data()) != 0;
}

// signatures[SigningUserId]["ed25519:<signing pub>"] = Signature (spec "Signing JSON").
void AddSignature(nlohmann::json &Object, const std::string &SigningUserId, const std::string &SigningPublicKey,
                  const std::string &Signature) {
    if(!Object.is_object()) {
        return;
    }
    if(!Object.contains("signatures") || !Object["signatures"].is_object()) {
        Object["signatures"] = nlohmann::json::object();
    }
    if(!Object["signatures"].contains(SigningUserId) || !Object["signatures"][SigningUserId].is_object()) {
        Object["signatures"][SigningUserId] = nlohmann::json::object();
    }
    Object["signatures"][SigningUserId]["ed25519:" + SigningPublicKey] = Signature;
}

// Spec-shaped CrossSigningKey: {user_id, usage, keys} with the key id embedding the public key.
nlohmann::json BuildCrossSigningKey(const std::string &UserId, const char *Usage, const std::string &PublicKey) {
    nlohmann::json Key;
    Key["user_id"] = UserId;
    Key["usage"] = nlohmann::json::array({Usage});
    Key["keys"] = nlohmann::json{{"ed25519:" + PublicKey, PublicKey}};
    return Key;
}

KeyEntry Ed25519Key(const nlohmann::json &KeyObject) {
    if(!KeyObject.is_object() || !KeyObject.contains("keys") || !KeyObject["keys"].is_object()) {
        return {};
    }
    const nlohmann::json &Keys = KeyObject["keys"];
    if(Keys.size() != 1) {
        return {}; // the spec requires exactly one property
    }
    const auto It = Keys.begin();
    if(!Util::StartsWith(It.key(), "ed25519:") || !It.value().is_string()) {
        return {};
    }
    KeyEntry Out;
    Out.Id = It.key().substr(8);
    Out.Value = It.value().get<std::string>();
    if(Out.Id.empty() || Out.Value.empty()) {
        return {};
    }
    return Out;
}

// device[Field][UserId][KeyId] as a string; empty when any level is missing.
std::string NestedString(const nlohmann::json &Object, const std::string &Field, const std::string &UserId, const std::string &KeyId) {
    if(!Object.is_object()) {
        return {};
    }
    const auto FieldIt = Object.find(Field);
    if(FieldIt == Object.end() || !FieldIt.value().is_object()) {
        return {};
    }
    const auto UserIt = FieldIt.value().find(UserId);
    if(UserIt == FieldIt.value().end() || !UserIt.value().is_object()) {
        return {};
    }
    const auto KeyIt = UserIt.value().find(KeyId);
    if(KeyIt == UserIt.value().end() || !KeyIt.value().is_string()) {
        return {};
    }
    return KeyIt.value().get<std::string>();
}

// /keys/query for one user (empty list = all devices); null on transport error, server
// failures or a non-object body.
nlohmann::json QueryUserKeys(Matrix::Client &Api, const std::string &UserId) {
    nlohmann::json Body;
    Body["device_keys"] = nlohmann::json{{UserId, nlohmann::json::array()}};
    Body["timeout"] = 10000; // spec recommends 10s for remote-server waits
    auto Response = Api.QueryKeys(Body);
    if(!Response.Ok || !Response.Value.is_object()) {
        return nlohmann::json();
    }
    if(Response.Value.contains("failures") && Response.Value["failures"].is_object() && !Response.Value["failures"].empty()) {
        return nlohmann::json(); // an unreachable server gives no trustworthy answer
    }
    return Response.Value;
}

// /keys/signatures/upload reports per-key rejections (e.g. M_INVALID_SIGNATURE).
bool UploadRejected(const nlohmann::json &Response) {
    if(!Response.is_object() || !Response.contains("failures")) {
        return false;
    }
    const nlohmann::json &Failures = Response["failures"];
    return Failures.is_object() && !Failures.empty();
}

} // namespace

CrossSigningManager::CrossSigningManager(Matrix::Client &Api, OlmAccount &Account) : m_Api(Api), m_Account(Account) {}

bool CrossSigningManager::IsSetUp() const {
    for(const char *Name : KeyNames) {
        if(FindPair(m_State, Name) == nullptr) {
            return false;
        }
    }
    return true;
}

std::string CrossSigningManager::GetMasterKeyFingerprint() const {
    const nlohmann::json *Master = FindPair(m_State, NameMaster);
    if(Master == nullptr) {
        return {};
    }
    const std::string PublicKey = Util::JsonGetString(*Master, "pub");
    return PublicKey.substr(0, 12);
}

bool CrossSigningManager::SetUpKeys(const std::string &Password) {
    const DeviceIdentity &Identity = m_Account.GetIdentity();
    if(Identity.UserId.empty()) {
        return false; // no olm account yet
    }
    if(!m_State.is_object()) {
        m_State = nlohmann::json::object();
    }
    for(const char *Name : KeyNames) {
        if(FindPair(m_State, Name) == nullptr) {
            GeneratePair(m_State, Name);
        }
    }

    const nlohmann::json *MasterEntry = FindPair(m_State, NameMaster);
    const nlohmann::json *SelfSigningEntry = FindPair(m_State, NameSelfSigning);
    const nlohmann::json *UserSigningEntry = FindPair(m_State, NameUserSigning);
    if(MasterEntry == nullptr || SelfSigningEntry == nullptr || UserSigningEntry == nullptr) {
        return false;
    }
    const Ed25519Pair Master = DecodePair(*MasterEntry);
    const Ed25519Pair SelfSigning = DecodePair(*SelfSigningEntry);
    const Ed25519Pair UserSigning = DecodePair(*UserSigningEntry);
    if(Master.Public.empty() || SelfSigning.Public.empty() || UserSigning.Public.empty()) {
        return false;
    }
    const std::string MasterPublicKey = Util::JsonGetString(*MasterEntry, "pub");
    const std::string SelfSigningPublicKey = Util::JsonGetString(*SelfSigningEntry, "pub");

    // Trust chain per spec: the master key signs itself and signs the SSK/USK; the server
    // rejects the request unless the SSK/USK carry a valid master signature.
    nlohmann::json MasterKey = BuildCrossSigningKey(Identity.UserId, NameMaster, MasterPublicKey);
    AddSignature(MasterKey, Identity.UserId, MasterPublicKey, SignObject(Master, MasterKey));
    nlohmann::json SelfSigningKey = BuildCrossSigningKey(Identity.UserId, NameSelfSigning, SelfSigningPublicKey);
    AddSignature(SelfSigningKey, Identity.UserId, MasterPublicKey, SignObject(Master, SelfSigningKey));
    nlohmann::json UserSigningKey = BuildCrossSigningKey(Identity.UserId, NameUserSigning, Util::JsonGetString(*UserSigningEntry, "pub"));
    AddSignature(UserSigningKey, Identity.UserId, MasterPublicKey, SignObject(Master, UserSigningKey));

    nlohmann::json SigningKeys;
    SigningKeys["master_key"] = MasterKey;
    SigningKeys["self_signing_key"] = SelfSigningKey;
    SigningKeys["user_signing_key"] = UserSigningKey;
    auto KeysResult = m_Api.UploadDeviceSigning(SigningKeys, Password);
    if(!KeysResult.Ok) {
        return false; // the generated keys stay in the state so a retry re-uploads the same set
    }

    // Cross-sign our own device so the session is trusted through the self-signing key.
    nlohmann::json DeviceKeys = m_Account.BuildDeviceKeys();
    if(!DeviceKeys.is_object() || DeviceKeys.empty()) {
        return false;
    }
    AddSignature(DeviceKeys, Identity.UserId, SelfSigningPublicKey, SignObject(SelfSigning, DeviceKeys));

    nlohmann::json Upload;
    Upload[Identity.UserId][Identity.DeviceId] = DeviceKeys;
    auto SignaturesResult = m_Api.UploadSignatures(Upload);
    return SignaturesResult.Ok && !UploadRejected(SignaturesResult.Value);
}

bool CrossSigningManager::SignDevice(const DeviceIdentity &Device) {
    const DeviceIdentity &Identity = m_Account.GetIdentity();
    if(Device.UserId.empty() || Device.UserId != Identity.UserId || Device.DeviceId.empty()) {
        return false; // only our own user's devices can be cross-signed here
    }
    if(Device.Ed25519Key.empty() || Device.Curve25519Key.empty() || !IsSetUp()) {
        return false;
    }
    const nlohmann::json *SelfSigningEntry = FindPair(m_State, NameSelfSigning);
    if(SelfSigningEntry == nullptr) {
        return false;
    }
    const Ed25519Pair SelfSigning = DecodePair(*SelfSigningEntry);
    if(SelfSigning.Public.empty()) {
        return false;
    }

    // The server requires the signed object to match the stored device_keys apart from
    // "signatures", so rebuild it exactly like OlmAccount::BuildDeviceKeys does.
    nlohmann::json DeviceKeys;
    DeviceKeys["user_id"] = Device.UserId;
    DeviceKeys["device_id"] = Device.DeviceId;
    DeviceKeys["algorithms"] = nlohmann::json::array({"m.olm.v1.curve25519-aes-sha2", "m.megolm.v1.aes-sha2"});
    nlohmann::json Keys;
    Keys["ed25519:" + Device.DeviceId] = Device.Ed25519Key;
    Keys["curve25519:" + Device.DeviceId] = Device.Curve25519Key;
    DeviceKeys["keys"] = Keys;
    AddSignature(DeviceKeys, Device.UserId, Util::JsonGetString(*SelfSigningEntry, "pub"), SignObject(SelfSigning, DeviceKeys));

    nlohmann::json Upload;
    Upload[Device.UserId][Device.DeviceId] = DeviceKeys;
    auto Result = m_Api.UploadSignatures(Upload);
    return Result.Ok && !UploadRejected(Result.Value);
}

bool CrossSigningManager::SignUser(const std::string &UserId) {
    if(UserId.empty() || !IsSetUp()) {
        return false;
    }
    const DeviceIdentity &Identity = m_Account.GetIdentity();
    const nlohmann::json *UserSigningEntry = FindPair(m_State, NameUserSigning);
    if(Identity.UserId.empty() || UserSigningEntry == nullptr) {
        return false;
    }
    const Ed25519Pair UserSigning = DecodePair(*UserSigningEntry);
    if(UserSigning.Public.empty()) {
        return false;
    }

    const nlohmann::json Query = QueryUserKeys(m_Api, UserId);
    if(!Query.is_object()) {
        return false;
    }
    const nlohmann::json Empty = nlohmann::json::object();
    const nlohmann::json &MasterKeys = Query.contains("master_keys") && Query["master_keys"].is_object() ? Query["master_keys"] : Empty;
    const auto MasterIt = MasterKeys.find(UserId);
    if(MasterIt == MasterKeys.end() || !MasterIt->is_object()) {
        return false; // no published master key to sign
    }

    nlohmann::json MasterKey = *MasterIt;
    const KeyEntry Entry = Ed25519Key(MasterKey);
    if(Entry.Id.empty()) {
        return false;
    }
    AddSignature(MasterKey, Identity.UserId, Util::JsonGetString(*UserSigningEntry, "pub"), SignObject(UserSigning, MasterKey));

    // The map key for a user's key is its raw public key id without the algorithm prefix
    // (spec /keys/signatures/upload example: {"@bob:example.com": {"bobs+base64+key": ...}}).
    nlohmann::json Upload;
    Upload[UserId][Entry.Id] = MasterKey;
    auto Result = m_Api.UploadSignatures(Upload);
    return Result.Ok && !UploadRejected(Result.Value);
}

bool CrossSigningManager::VerifyRemoteDevice(const std::string &UserId, const std::string &DeviceId) {
    if(UserId.empty() || DeviceId.empty()) {
        return false;
    }
    const nlohmann::json Query = QueryUserKeys(m_Api, UserId);
    if(!Query.is_object()) {
        return false;
    }

    const nlohmann::json Empty = nlohmann::json::object();
    const nlohmann::json &DeviceKeysOf = Query.contains("device_keys") && Query["device_keys"].is_object() ? Query["device_keys"] : Empty;
    const auto UserIt = DeviceKeysOf.find(UserId);
    if(UserIt == DeviceKeysOf.end() || !UserIt->is_object()) {
        return false;
    }
    const auto DeviceIt = UserIt->find(DeviceId);
    if(DeviceIt == UserIt->end() || !DeviceIt->is_object()) {
        return false;
    }
    const nlohmann::json &Device = *DeviceIt;

    const nlohmann::json &SelfSigningKeys =
        Query.contains("self_signing_keys") && Query["self_signing_keys"].is_object() ? Query["self_signing_keys"] : Empty;
    const auto SskIt = SelfSigningKeys.find(UserId);
    if(SskIt == SelfSigningKeys.end()) {
        return false;
    }
    const KeyEntry SelfSigning = Ed25519Key(*SskIt);
    if(SelfSigning.Id.empty() || SelfSigning.Value.empty()) {
        return false;
    }

    const nlohmann::json &Keys = Device.contains("keys") && Device["keys"].is_object() ? Device["keys"] : Empty;
    const std::string OwnPublicKey = Util::JsonGetString(Keys, "ed25519:" + DeviceId);
    const std::string OwnSignature = NestedString(Device, "signatures", UserId, "ed25519:" + DeviceId);
    const std::string CrossSignature = NestedString(Device, "signatures", UserId, "ed25519:" + SelfSigning.Id);
    if(OwnPublicKey.empty() || OwnSignature.empty() || CrossSignature.empty()) {
        return false;
    }

    const std::string Message = Util::CanonicalJson(WithoutSignatures(Device));
    if(!VerifySignature(OwnPublicKey, Message, OwnSignature)) {
        return false; // (a) the device did not self-sign its own keys
    }
    return VerifySignature(SelfSigning.Value, Message, CrossSignature); // (b) no valid self-signing-key cross-signature
}

nlohmann::json CrossSigningManager::ExportState() const {
    return m_State.is_object() ? m_State : nlohmann::json::object();
}

void CrossSigningManager::ImportState(const nlohmann::json &State) {
    m_State = nlohmann::json::object();
    if(!State.is_object()) {
        return;
    }
    for(const char *Name : KeyNames) {
        const auto It = State.find(Name);
        if(It == State.end() || !It->is_object()) {
            continue;
        }
        const Ed25519Pair Pair = DecodePair(*It);
        if(Pair.Public.empty()) {
            continue; // drop entries that would never verify or sign
        }
        m_State[Name] = nlohmann::json{{"priv", Util::JsonGetString(*It, "priv")}, {"pub", Util::JsonGetString(*It, "pub")}};
    }
}

} // namespace NxMatrix::E2ee