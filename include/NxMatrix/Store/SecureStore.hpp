#pragma once
#include "NxMatrix/Store/Vault.hpp"
#include "NxMatrix/Types.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace NxMatrix::Store {

// Everything the app remembers about the logged-in account. Always stored
// sealed with the Vault (access token, E2EE pickles, sync state...).
struct AccountData {
    std::string UserId;
    std::string DeviceId;
    std::string HomeserverUrl;
    std::string AccessToken;
    std::string RefreshToken;
    std::string SyncNextBatch;
    std::string DisplayName;
    std::string AvatarMxc;
    std::string Presence = "online";
    nlohmann::json OlmState = nlohmann::json::object(); // opaque E2EE pickles (see E2eeManager)
    nlohmann::json Settings = nlohmann::json::object(); // UI preferences

    nlohmann::json ToJson() const;
    static AccountData FromJson(const nlohmann::json &Json);
};

// The ONLY data kept in clear on the SD card (plus the avatar image file):
// the Matrix user ID and the avatar file name. This lets the login screen
// greet the user without unlocking the vault.
struct PublicProfile {
    std::string UserId;
    std::string AvatarFileName;

    nlohmann::json ToJson() const;
    static PublicProfile FromJson(const nlohmann::json &Json);
};

// SD card persistence. Layout of GetRootPath():
//   Account.nxmc    sealed AccountData
//   Profile.json    plaintext PublicProfile (user id + avatar name only)
//   Avatar.img      plaintext profile picture
//   Cache/Media/    sealed media cache (key = mxc + size)
class SecureStore {
public:
    static std::string GetRootPath();

    // Creates the directories. Returns false when the SD card is unavailable.
    bool Initialize();

    bool HasAccount() const;
    bool SaveAccount(const Vault &VaultRef, const AccountData &Account);
    bool LoadAccount(const Vault &VaultRef, AccountData &OutAccount);
    // Reads the KDF parameters from the sealed account header BEFORE the vault
    // can be opened (UnlockLayout re-derives the key from them).
    bool ReadAccountKdf(Crypto::SaltBytes &OutSalt, std::uint32_t &OutIterations) const;

    bool SavePublicProfile(const PublicProfile &Profile);
    bool LoadPublicProfile(PublicProfile &OutProfile) const;

    void SaveAvatarImage(const ByteArray &Image);
    ByteArray LoadAvatarImage() const;

    // UI theme preference, kept in clear so it also applies to the unlock
    // screen. Returns false when the user never chose (use the console theme).
    bool SaveThemeDark(bool Dark);
    bool LoadThemeDark(bool &OutDark) const;

    bool SaveMediaCache(const Vault &VaultRef, const std::string &CacheKey, const ByteArray &Data);
    bool LoadMediaCache(const Vault &VaultRef, const std::string &CacheKey, ByteArray &OutData) const;
    void ClearMediaCache();

    // Deletes every stored file (logout + wipe).
    void Wipe();
};

} // namespace NxMatrix::Store