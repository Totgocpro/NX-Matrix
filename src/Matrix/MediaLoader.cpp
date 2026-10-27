// mxc:// loader: sealed SD cache first, network fallback, results posted to
// the UI thread through Core::Dispatcher.
#include "NxMatrix/Matrix/MediaLoader.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Crypto/AesCtr.hpp"
#include "NxMatrix/Crypto/Hash.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Util/Base64.hpp"
#include "NxMatrix/Util/Json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

namespace NxMatrix::Matrix {

MediaLoader::MediaLoader(Client &Api, Store::SecureStore &StoreRef, const Store::Vault &VaultRef)
    : m_Api(Api), m_Store(StoreRef), m_Vault(VaultRef) {}

void MediaLoader::LoadMxc(const std::string &MxcUrl, int ThumbnailWidth, int ThumbnailHeight, Callback OnLoaded) {
    // Exactly-once guard: the cache and network paths must not both fire.
    auto Done = std::make_shared<std::atomic<bool>>(false);
    auto Fire = [Done, OnLoaded](bool Success, const ByteArray &Data) {
        if(Done->exchange(true)) {
            return;
        }
        Core::Dispatcher::Instance().Post([OnLoaded, Success, Data]() { OnLoaded(Success, Data); });
    };

    if(MxcUrl.empty()) {
        Fire(false, {});
        return;
    }

    const std::string CacheKey = MxcUrl + "#" + std::to_string(ThumbnailWidth) + "x" + std::to_string(ThumbnailHeight);
    Core::JobQueue::Instance().Enqueue([this, MxcUrl, ThumbnailWidth, ThumbnailHeight, CacheKey, Fire]() {
        ByteArray Cached;
        if(m_Store.LoadMediaCache(m_Vault, CacheKey, Cached)) {
            Fire(true, Cached);
            return;
        }
        Result<ByteArray> Download = m_Api.DownloadMedia(MxcUrl, ThumbnailWidth, ThumbnailHeight);
        if(!Download.Ok) {
            Fire(false, {});
            return;
        }
        // Best-effort persist: a full/blocked SD still serves the bytes.
        m_Store.SaveMediaCache(m_Vault, CacheKey, Download.Value);
        Fire(true, Download.Value);
    });
}

void MediaLoader::LoadEncryptedMxc(const std::string &MxcUrl, const nlohmann::json &EncryptedFile, Callback OnLoaded) {
    // Exactly-once guard, same as LoadMxc.
    auto Done = std::make_shared<std::atomic<bool>>(false);
    auto Fire = [Done, OnLoaded](bool Success, const ByteArray &Data) {
        if(Done->exchange(true)) {
            return;
        }
        Core::Dispatcher::Instance().Post([OnLoaded, Success, Data]() { OnLoaded(Success, Data); });
    };

    // The JWK key, IV and ciphertext hash must all decode before anything is
    // fetched: without them the download could never be turned into pixels.
    const auto KeyIt = EncryptedFile.find("key");
    const auto HashesIt = EncryptedFile.find("hashes");
    const std::string KeyB64 = KeyIt != EncryptedFile.end() && KeyIt->is_object() ? Util::JsonGetString(*KeyIt, "k") : "";
    const std::string HashB64 = HashesIt != EncryptedFile.end() && HashesIt->is_object() ? Util::JsonGetString(*HashesIt, "sha256") : "";
    const ByteArray KeyBytesRaw = Util::Base64Decode(KeyB64);
    const ByteArray IvRaw = Util::Base64Decode(Util::JsonGetString(EncryptedFile, "iv"));
    const ByteArray ExpectedHash = Util::Base64Decode(HashB64);
    if(MxcUrl.empty() || KeyBytesRaw.size() != 32 || IvRaw.size() != 16 || ExpectedHash.size() != 32) {
        Fire(false, {});
        return;
    }
    Crypto::KeyBytes Key{};
    Crypto::SaltBytes Counter{}; // 16 bytes, same layout as the spec's IV
    std::copy(KeyBytesRaw.begin(), KeyBytesRaw.end(), Key.begin());
    std::copy(IvRaw.begin(), IvRaw.end(), Counter.begin());

    const std::string CacheKey = MxcUrl + "#enc";
    Core::JobQueue::Instance().Enqueue([this, MxcUrl, CacheKey, Key, Counter, ExpectedHash, Fire]() {
        ByteArray Cached;
        if(m_Store.LoadMediaCache(m_Vault, CacheKey, Cached)) {
            Fire(true, Cached);
            return;
        }
        // Whole file: a server cannot produce a thumbnail of ciphertext.
        Result<ByteArray> Download = m_Api.DownloadMedia(MxcUrl, 0, 0);
        if(!Download.Ok) {
            Fire(false, {});
            return;
        }
        // The SHA-256 covers the ciphertext: refuse tampered or truncated
        // bytes before the unauthenticated stream cipher touches them.
        if(Crypto::Sha256(Download.Value) != ExpectedHash) {
            Fire(false, {});
            return;
        }
        ByteArray Plain;
        if(!Crypto::AesCtr256Crypt(Key, Counter, Download.Value, Plain)) {
            Fire(false, {});
            return;
        }
        // Best-effort persist: a full/blocked SD still serves the bytes.
        m_Store.SaveMediaCache(m_Vault, CacheKey, Plain);
        Fire(true, Plain);
    });
}

void MediaLoader::ClearCache() {
    m_Store.ClearMediaCache();
}

} // namespace NxMatrix::Matrix