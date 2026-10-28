#include "NxMatrix/Store/SecureStore.hpp"

#include "NxMatrix/Crypto/Hash.hpp"
#include "NxMatrix/Util/Json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace NxMatrix::Store {

namespace {

constexpr char RootPath[] = "sdmc:/switch/NX-Matrix";
constexpr char Magic[] = "NXMV";
constexpr std::uint16_t FormatVersion = 1;
constexpr std::size_t AccountHeaderSize = 40;

bool MakeDir(const std::string &Path) {
    if(mkdir(Path.c_str(), 0777) == 0) {
        return true;
    }
    return errno == EEXIST;
}

bool ReadWholeFile(const std::string &Path, ByteArray &Out) {
    std::FILE *File = std::fopen(Path.c_str(), "rb");
    if(File == nullptr) {
        return false;
    }
    Out.clear();
    std::uint8_t Buffer[4096];
    std::size_t Read = 0;
    while((Read = std::fread(Buffer, 1, sizeof(Buffer), File)) > 0) {
        Out.insert(Out.end(), Buffer, Buffer + Read);
    }
    const bool Failed = std::ferror(File) != 0;
    std::fclose(File);
    if(Failed) {
        Out.clear();
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::string &Path, std::string &Out) {
    ByteArray Bytes;
    if(!ReadWholeFile(Path, Bytes)) {
        return false;
    }
    Out.assign(Bytes.begin(), Bytes.end());
    return true;
}

// Write to a temp file, then swap it in: a crash mid-write never truncates
// the previous good file (FAT rename does not overwrite, so drop first).
bool WriteAtomic(const std::string &Path, const std::uint8_t *Data, std::size_t Size) {
    const std::string TempPath = Path + ".tmp";
    std::FILE *File = std::fopen(TempPath.c_str(), "wb");
    if(File == nullptr) {
        return false;
    }
    bool Ok = (Size == 0) || std::fwrite(Data, 1, Size, File) == Size;
    if(std::fclose(File) != 0) {
        Ok = false;
    }
    if(!Ok) {
        std::remove(TempPath.c_str());
        return false;
    }
    std::remove(Path.c_str());
    if(std::rename(TempPath.c_str(), Path.c_str()) != 0) {
        std::remove(TempPath.c_str());
        return false;
    }
    return true;
}

bool WriteAtomic(const std::string &Path, const ByteArray &Data) {
    return WriteAtomic(Path, Data.data(), Data.size());
}

bool WriteAtomic(const std::string &Path, const std::string &Data) {
    return WriteAtomic(Path, reinterpret_cast<const std::uint8_t *>(Data.data()), Data.size());
}

std::string HexEncode(const std::uint8_t *Data, std::size_t Size) {
    static const char kHex[] = "0123456789abcdef";
    std::string Out;
    Out.reserve(Size * 2);
    for(std::size_t I = 0; I < Size; ++I) {
        Out.push_back(kHex[Data[I] >> 4]);
        Out.push_back(kHex[Data[I] & 0x0F]);
    }
    return Out;
}

void RemoveTree(const std::string &Path) {
    DIR *Dir = opendir(Path.c_str());
    if(Dir != nullptr) {
        while(true) {
            const dirent *Entry = readdir(Dir);
            if(Entry == nullptr) {
                break;
            }
            if(std::strcmp(Entry->d_name, ".") == 0 || std::strcmp(Entry->d_name, "..") == 0) {
                continue;
            }
            const std::string Child = Path + "/" + Entry->d_name;
            struct stat St{};
            if(stat(Child.c_str(), &St) == 0 && S_ISDIR(St.st_mode)) {
                RemoveTree(Child);
            } else {
                std::remove(Child.c_str());
            }
        }
        closedir(Dir);
    }
    rmdir(Path.c_str());
}

// Empties Path but keeps the directory itself (media cache must stay writable).
void ClearDirectory(const std::string &Path) {
    DIR *Dir = opendir(Path.c_str());
    if(Dir == nullptr) {
        return;
    }
    while(true) {
        const dirent *Entry = readdir(Dir);
        if(Entry == nullptr) {
            break;
        }
        if(std::strcmp(Entry->d_name, ".") == 0 || std::strcmp(Entry->d_name, "..") == 0) {
            continue;
        }
        const std::string Child = Path + "/" + Entry->d_name;
        struct stat St{};
        if(stat(Child.c_str(), &St) == 0 && S_ISDIR(St.st_mode)) {
            RemoveTree(Child);
        } else {
            std::remove(Child.c_str());
        }
    }
    closedir(Dir);
}

std::string MediaCachePath(const std::string &CacheKey) {
    const ByteArray Digest = Crypto::Sha256(CacheKey);
    return std::string(RootPath) + "/Cache/Media/" + HexEncode(Digest.data(), Digest.size()) + ".nxmc";
}

std::string Serialize(const nlohmann::json &Value) {
    return Value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

nlohmann::json AccountData::ToJson() const {
    nlohmann::json Json = nlohmann::json::object();
    Json["userId"] = UserId;
    Json["deviceId"] = DeviceId;
    Json["homeserverUrl"] = HomeserverUrl;
    Json["accessToken"] = AccessToken;
    Json["refreshToken"] = RefreshToken;
    Json["syncNextBatch"] = SyncNextBatch;
    Json["displayName"] = DisplayName;
    Json["avatarMxc"] = AvatarMxc;
    Json["presence"] = Presence;
    Json["olmState"] = OlmState;
    Json["settings"] = Settings;
    return Json;
}

AccountData AccountData::FromJson(const nlohmann::json &Json) {
    AccountData Out;
    Out.UserId = Util::JsonGetString(Json, "userId");
    Out.DeviceId = Util::JsonGetString(Json, "deviceId");
    Out.HomeserverUrl = Util::JsonGetString(Json, "homeserverUrl");
    Out.AccessToken = Util::JsonGetString(Json, "accessToken");
    Out.RefreshToken = Util::JsonGetString(Json, "refreshToken");
    Out.SyncNextBatch = Util::JsonGetString(Json, "syncNextBatch");
    Out.DisplayName = Util::JsonGetString(Json, "displayName");
    Out.AvatarMxc = Util::JsonGetString(Json, "avatarMxc");
    Out.Presence = Util::JsonGetString(Json, "presence", Out.Presence);
    if(Json.is_object()) {
        const auto OlmIt = Json.find("olmState");
        if(OlmIt != Json.end() && (OlmIt->is_object() || OlmIt->is_array())) {
            Out.OlmState = *OlmIt;
        }
        const auto SettingsIt = Json.find("settings");
        if(SettingsIt != Json.end() && (SettingsIt->is_object() || SettingsIt->is_array())) {
            Out.Settings = *SettingsIt;
        }
    }
    return Out;
}

nlohmann::json PublicProfile::ToJson() const {
    nlohmann::json Json = nlohmann::json::object();
    Json["userId"] = UserId;
    Json["avatarFileName"] = AvatarFileName;
    return Json;
}

PublicProfile PublicProfile::FromJson(const nlohmann::json &Json) {
    PublicProfile Out;
    Out.UserId = Util::JsonGetString(Json, "userId");
    Out.AvatarFileName = Util::JsonGetString(Json, "avatarFileName");
    return Out;
}

std::string SecureStore::GetRootPath() {
    return RootPath;
}

bool SecureStore::Initialize() {
    const std::string Root = GetRootPath();
    if(!MakeDir(Root)) {
        return false; // SD card unavailable
    }
    if(!MakeDir(Root + "/Cache")) {
        return false;
    }
    if(!MakeDir(Root + "/Cache/Media")) {
        return false;
    }
    return true;
}

bool SecureStore::HasAccount() const {
    struct stat St{};
    const std::string Path = GetRootPath() + "/Account.nxmc";
    return stat(Path.c_str(), &St) == 0 && S_ISREG(St.st_mode);
}

bool SecureStore::SaveAccount(const Vault &VaultRef, const AccountData &Account) {
    const std::string Text = Serialize(Account.ToJson());
    const ByteArray Sealed = VaultRef.Seal(ByteArray(Text.begin(), Text.end()));
    if(Sealed.empty()) {
        return false; // vault not open
    }
    return WriteAtomic(GetRootPath() + "/Account.nxmc", Sealed);
}

bool SecureStore::LoadAccount(const Vault &VaultRef, AccountData &OutAccount) {
    ByteArray Sealed;
    if(!ReadWholeFile(GetRootPath() + "/Account.nxmc", Sealed)) {
        return false;
    }
    ByteArray Plain;
    if(!VaultRef.Open(Sealed, Plain)) {
        return false;
    }
    const std::string Text(Plain.begin(), Plain.end());
    const nlohmann::json Json = nlohmann::json::parse(Text, nullptr, false);
    if(Json.is_discarded() || !Json.is_object()) {
        return false;
    }
    OutAccount = AccountData::FromJson(Json);
    return true;
}

bool SecureStore::ReadAccountKdf(Crypto::SaltBytes &OutSalt, std::uint32_t &OutIterations) const {
    const std::string Path = GetRootPath() + "/Account.nxmc";
    std::FILE *File = std::fopen(Path.c_str(), "rb");
    if(File == nullptr) {
        return false;
    }
    std::uint8_t Header[AccountHeaderSize];
    const std::size_t Read = std::fread(Header, 1, sizeof(Header), File);
    std::fclose(File);
    if(Read != sizeof(Header)) {
        return false;
    }
    if(std::memcmp(Header, Magic, 4) != 0) {
        return false;
    }
    const std::uint16_t Version = static_cast<std::uint16_t>(Header[4] | (Header[5] << 8));
    if(Version != FormatVersion) {
        return false;
    }
    OutIterations = static_cast<std::uint32_t>(Header[8]) | (static_cast<std::uint32_t>(Header[9]) << 8) |
                    (static_cast<std::uint32_t>(Header[10]) << 16) | (static_cast<std::uint32_t>(Header[11]) << 24);
    std::copy_n(Header + 12, OutSalt.size(), OutSalt.begin());
    return true;
}

bool SecureStore::SavePublicProfile(const PublicProfile &Profile) {
    return WriteAtomic(GetRootPath() + "/Profile.json", Serialize(Profile.ToJson()));
}

bool SecureStore::LoadPublicProfile(PublicProfile &OutProfile) const {
    std::string Text;
    if(!ReadWholeFile(GetRootPath() + "/Profile.json", Text)) {
        return false;
    }
    const nlohmann::json Json = nlohmann::json::parse(Text, nullptr, false);
    if(Json.is_discarded() || !Json.is_object()) {
        return false;
    }
    OutProfile = PublicProfile::FromJson(Json);
    return true;
}

void SecureStore::SaveAvatarImage(const ByteArray &Image) {
    WriteAtomic(GetRootPath() + "/Avatar.img", Image);
}

ByteArray SecureStore::LoadAvatarImage() const {
    ByteArray Image;
    ReadWholeFile(GetRootPath() + "/Avatar.img", Image);
    return Image;
}

bool SecureStore::SaveThemeDark(const bool Dark) {
    nlohmann::json Json = nlohmann::json::object();
    Json["darkMode"] = Dark;
    return WriteAtomic(GetRootPath() + "/Theme.json", Serialize(Json));
}

bool SecureStore::LoadThemeDark(bool &OutDark) const {
    std::string Text;
    if(!ReadWholeFile(GetRootPath() + "/Theme.json", Text)) {
        return false;
    }
    const nlohmann::json Json = nlohmann::json::parse(Text, nullptr, false);
    if(Json.is_discarded() || !Json.is_object()) {
        return false;
    }
    const auto It = Json.find("darkMode");
    if(It == Json.end() || !It->is_boolean()) {
        return false;
    }
    OutDark = It->get<bool>();
    return true;
}

bool SecureStore::SaveMediaCache(const Vault &VaultRef, const std::string &CacheKey, const ByteArray &Data) {
    const ByteArray Sealed = VaultRef.Seal(Data);
    if(Sealed.empty()) {
        return false;
    }
    return WriteAtomic(MediaCachePath(CacheKey), Sealed);
}

bool SecureStore::LoadMediaCache(const Vault &VaultRef, const std::string &CacheKey, ByteArray &OutData) const {
    ByteArray Sealed;
    if(!ReadWholeFile(MediaCachePath(CacheKey), Sealed)) {
        return false;
    }
    return VaultRef.Open(Sealed, OutData);
}

void SecureStore::ClearMediaCache() {
    ClearDirectory(GetRootPath() + "/Cache/Media");
}

void SecureStore::Wipe() {
    RemoveTree(GetRootPath());
}

} // namespace NxMatrix::Store