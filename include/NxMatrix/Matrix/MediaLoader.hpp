#pragma once
#include "NxMatrix/Store/SecureStore.hpp"
#include "NxMatrix/Store/Vault.hpp"
#include "NxMatrix/Types.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace NxMatrix::Matrix {

class Client;

// Media pipeline for mxc:// URLs: sealed SD cache first, then the network.
// Callbacks are marshalled to the UI thread through Core::Dispatcher.
class MediaLoader {
public:
    using Callback = std::function<void(bool Success, const ByteArray &Data)>;

    MediaLoader(Client &Api, Store::SecureStore &StoreRef, const Store::Vault &VaultRef);

    // Thumbnail when width/height > 0. CacheKey = "<mxc>#<w>x<h>".
    void LoadMxc(const std::string &MxcUrl, int ThumbnailWidth, int ThumbnailHeight, Callback OnLoaded);
    // Encrypted attachment (spec "Encrypted attachments"): downloads the
    // ciphertext whole (servers cannot thumbnail it), verifies its SHA-256,
    // AES-256-CTR decrypts with the JWK in EncryptedFile and caches the
    // plaintext under "<mxc>#enc". Callback gets the plaintext bytes.
    void LoadEncryptedMxc(const std::string &MxcUrl, const nlohmann::json &EncryptedFile, Callback OnLoaded);
    void ClearCache();

private:
    Client &m_Api;
    Store::SecureStore &m_Store;
    const Store::Vault &m_Vault;
};

} // namespace NxMatrix::Matrix