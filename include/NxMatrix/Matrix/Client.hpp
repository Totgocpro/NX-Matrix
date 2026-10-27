#pragma once
#include "NxMatrix/Matrix/ApiError.hpp"
#include "NxMatrix/Matrix/Models.hpp"
#include "NxMatrix/Types.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace NxMatrix::Matrix {

struct ServerVersions {
    std::vector<std::string> Versions;
    std::map<std::string, bool> UnstableFeatures;

    bool Supports(const std::string &Version) const;
};

struct LoginResult {
    std::string AccessToken;
    std::string RefreshToken;
    std::string DeviceId;
    std::string UserId;
    std::int64_t ExpiresInMs = 0;
};

struct RoomCreateParams {
    std::string Name;
    std::string Topic;
    std::string RoomAlias;
    std::vector<std::string> InviteUserIds;
    bool IsDirect = false;
    bool IsPublic = false;
    bool IsEncrypted = true;
    // creation_content type m.space (spec v1.19 §Spaces): a room grouping
    // other rooms, created with events_default 100 so only admins post.
    bool IsSpace = false;
};

// One room entry of the space hierarchy (GET /rooms/{roomId}/hierarchy).
struct SpaceHierarchyRoom {
    std::string RoomId;
    std::string Name;
    std::string Topic;
    std::string AvatarMxc;
    std::string CanonicalAlias;
    std::string RoomType; // m.room.create type, "m.space" for subspaces
    std::string JoinRule; // empty = assumed public per spec
    std::int64_t NumJoinedMembers = 0;
    bool WorldReadable = false;
    std::int64_t ChildCount = 0; // valid m.space.child edges of this room
};

struct SpaceHierarchyPage {
    std::vector<SpaceHierarchyRoom> Rooms; // depth-first, requested space may repeat children
    std::string NextBatch;                 // empty = no further pages
};

struct BackfillResult {
    std::vector<Event> Events;
    std::string PrevBatch;
    std::string NextBatch;
};

struct SearchResult {
    std::vector<Event> Events;
    std::size_t ResultCount = 0;
};

// Blocking Matrix Client-Server API (https://spec.matrix.org/v1.19/client-server-api/)
// wrapper over Net::HttpClient. Every call runs on a worker thread.
class Client {
public:
    // --- Setup / session state ---
    void Configure(const std::string &BaseUrl);
    bool IsConfigured() const;
    const std::string &GetBaseUrl() const;
    void SetSession(const std::string &AccessToken, const std::string &DeviceId, const std::string &UserId);
    void ClearSession();
    bool HasSession() const;
    const std::string &GetAccessToken() const;
    const std::string &GetDeviceId() const;
    const std::string &GetUserId() const;

    // --- Discovery / authentication ---
    Result<ServerVersions> GetVersions() const;
    Result<nlohmann::json> GetLoginFlows() const;
    Result<LoginResult> LoginPassword(const std::string &User, const std::string &Password) const;
    // Registration with UIAA: handles m.login.dummy and m.login.terms internally;
    // fails with a clear message when the server demands captcha/3PID stages.
    Result<LoginResult> Register(const std::string &User, const std::string &Password) const;
    Result<LoginResult> RefreshSession(const std::string &RefreshToken) const;
    Status Logout() const;
    Status LogoutAll() const;
    Result<nlohmann::json> WhoAmI() const;

    // --- Sync / rooms / messages ---
    Result<nlohmann::json> Sync(const std::string &Since, int TimeoutSec) const;
    Result<std::vector<std::string>> GetJoinedRooms() const;
    Result<nlohmann::json> GetRoomState(const std::string &RoomId) const;
    // GET /rooms/{roomId}/hierarchy (spec v1.19 §Spaces): one page of the
    // space tree, depth-first. From = next_batch of the previous page.
    Result<SpaceHierarchyPage> GetSpaceHierarchy(const std::string &RoomId, const std::string &From = {}, int Limit = 0) const;
    // GET /rooms/{roomId}/state/{eventType}[/{stateKey}]: the content of one
    // state event (M_NOT_FOUND when the room has none).
    Result<nlohmann::json> GetStateEvent(const std::string &RoomId, const std::string &EventType, const std::string &StateKey = {}) const;
    Result<BackfillResult> GetMessages(const std::string &RoomId, const std::string &From, const std::string &To, bool Backward,
                                       int Limit) const;
    Result<std::string> SendEvent(const std::string &RoomId, const std::string &EventType, const nlohmann::json &Content,
                                  const std::string &TransactionId) const;
    Result<std::string> SendStateEvent(const std::string &RoomId, const std::string &EventType, const std::string &StateKey,
                                       const nlohmann::json &Content) const;
    Status RedactEvent(const std::string &RoomId, const std::string &EventId, const std::string &Reason,
                       const std::string &TransactionId) const;
    Status SetTyping(const std::string &RoomId, bool Typing, int TimeoutSec) const;
    Status SendReadReceipt(const std::string &RoomId, const std::string &EventId) const;
    Status SetReadMarkers(const std::string &RoomId, const std::string &FullyReadEventId, const std::string &ReadEventId) const;

    // --- Room management ---
    // ViaServers: `via` routing hints (spec §Routing) used when the target is
    // a room ID the local server cannot resolve on its own.
    Result<std::string> JoinRoom(const std::string &RoomIdOrAlias, const std::vector<std::string> &ViaServers = {}) const;
    Status LeaveRoom(const std::string &RoomId, const std::string &Reason = {}) const;
    Status InviteUser(const std::string &RoomId, const std::string &UserId) const;
    Status KickUser(const std::string &RoomId, const std::string &UserId, const std::string &Reason = {}) const;
    Status BanUser(const std::string &RoomId, const std::string &UserId, const std::string &Reason = {}) const;
    Status UnbanUser(const std::string &RoomId, const std::string &UserId) const;
    Status ForgetRoom(const std::string &RoomId) const;
    Result<std::string> CreateRoom(const RoomCreateParams &Params) const;
    Result<std::map<std::string, RoomMember>> GetJoinedMembers(const std::string &RoomId) const;

    // --- Tags / account data ---
    Status SetTag(const std::string &RoomId, const std::string &Tag, double Order) const;
    Status RemoveTag(const std::string &RoomId, const std::string &Tag) const;
    Result<nlohmann::json> GetAccountData(const std::string &Type) const;
    Status SetAccountData(const std::string &Type, const nlohmann::json &Content) const;

    // --- Profile / presence ---
    Result<UserProfile> GetProfile(const std::string &UserId) const;
    Status SetDisplayName(const std::string &DisplayName) const;
    Status SetAvatarMxc(const std::string &AvatarMxc) const;
    Status SetPresence(const std::string &Presence) const;

    // --- Media ---
    Result<std::string> UploadMedia(const ByteArray &Data, const std::string &ContentType, const std::string &FileName) const;
    // Thumbnail when width/height > 0, full media otherwise. Handles mxc:// URLs.
    Result<ByteArray> DownloadMedia(const std::string &MxcUrl, int ThumbnailWidth, int ThumbnailHeight) const;

    // --- Search / devices ---
    Result<SearchResult> SearchMessages(const std::string &Term, int Limit) const;
    Result<std::vector<DeviceInfo>> GetDevices() const;
    // Password-authenticated (UIAA m.login.password) device deletion.
    Status DeleteDevice(const std::string &DeviceId, const std::string &Password) const;

    // --- E2EE endpoints ---
    Result<nlohmann::json> UploadKeys(const nlohmann::json &KeysBody) const;
    Result<nlohmann::json> QueryKeys(const nlohmann::json &QueryBody) const;
    Result<nlohmann::json> ClaimKeys(const nlohmann::json &ClaimBody) const;
    Status SendToDevice(const std::string &EventType, const nlohmann::json &Messages, const std::string &TransactionId) const;
    Result<std::vector<std::string>> GetKeyChanges(const std::string &FromToken, const std::string &ToToken) const;

    // --- Key backup (4S): /room_keys/*, spec "Backing up room keys" ---
    Result<nlohmann::json> GetRoomKeysLatestVersion() const;
    Result<std::vector<std::string>> GetRoomKeysVersions() const;
    Result<std::string> CreateRoomKeysVersion(const nlohmann::json &VersionInfo) const;
    // Empty RoomId/SessionId fetches everything under /keys; Version empty = latest.
    Result<nlohmann::json> GetRoomKeysSessions(const std::string &Version, const std::string &RoomId, const std::string &SessionId) const;
    Status UploadRoomKeys(const std::string &Version, const nlohmann::json &Sessions) const;

    // --- Cross-signing ---
    // POST /keys/device_signing/upload with password UIAA staging (like DeleteDevice);
    // SigningKeys = {master_key, self_signing_key, user_signing_key}.
    Result<nlohmann::json> UploadDeviceSigning(const nlohmann::json &SigningKeys, const std::string &Password) const;
    // POST /keys/signatures/upload: {user_id: {device_id: {signed object}}}.
    Result<nlohmann::json> UploadSignatures(const nlohmann::json &Signatures) const;

private:
    std::string m_BaseUrl;
    std::string m_AccessToken;
    std::string m_DeviceId;
    std::string m_UserId;
};

} // namespace NxMatrix::Matrix