#pragma once
#include "NxMatrix/Types.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace NxMatrix::Matrix {

enum class MembershipState {
    None,
    Invite,
    Join,
    Knock,
    Leave,
    Ban,
};

MembershipState MembershipFromString(const std::string &Value);
const char *MembershipToString(MembershipState Value);

// One Matrix event. Content holds the decrypted payload for E2EE events
// (IsDecrypted) or the raw content otherwise.
struct Event {
    std::string EventId;
    std::string RoomId;
    std::string Sender;
    std::string Type;
    std::string StateKey;
    std::int64_t Timestamp = 0;
    nlohmann::json Content = nlohmann::json::object();
    nlohmann::json Unsigned = nlohmann::json::object();
    bool IsDecrypted = false;
    bool IsDecryptionFailed = false;
    bool IsRedacted = false;
    bool IsLocalEcho = false;
    std::string TransactionId;

    static Event FromJson(const nlohmann::json &Json, const std::string &RoomId);
};

struct ReactionGroup {
    std::string Key;
    std::vector<std::string> Senders;
    bool Mine = false;
};

struct RoomMember {
    std::string UserId;
    std::string DisplayName;
    std::string AvatarMxc;
    MembershipState Membership = MembershipState::None;
    int PowerLevel = 0;
};

struct UserInfo {
    std::string UserId;
    std::string DisplayName;
    std::string AvatarMxc;
    std::string Presence;
    std::int64_t LastActiveAgoMs = 0;
};

struct UserProfile {
    std::string UserId;
    std::string DisplayName;
    std::string AvatarMxc;
};

struct DeviceInfo {
    std::string DeviceId;
    std::string DisplayName;
    std::string LastSeenIp;
    std::int64_t LastSeenTs = 0;
};

struct PowerLevels {
    // Room version 12+ creators hold an immutable, infinitely high level
    // (spec v1.19 §Permissions): every required level passes against it.
    static constexpr int InfiniteLevel = INT32_MAX;

    int UsersDefault = 0;
    int EventsDefault = 0;
    int StateDefault = 50;
    int Ban = 50;
    int Kick = 50;
    int Redact = 50;
    int Invite = 0;
    int Name = 50;
    int Topic = 50;
    std::map<std::string, int> Users;
    std::map<std::string, int> Events;

    // Room identity, filled by Session from m.room.create: the creator is not
    // derivable from the power levels event itself (absent in room v12+).
    std::string CreatorUserId;
    int RoomVersion = 1;
    bool HasPowerLevelsEvent = false;

    static PowerLevels FromJson(const nlohmann::json &Json);
    int GetUserLevel(const std::string &UserId) const;
    bool CanSendEvent(const std::string &EventType, int UserLevel) const;
    bool CanSendState(const std::string &EventType, int UserLevel) const;
};

// One m.space.child edge of a space-room (state key = child room id).
struct SpaceChild {
    std::string RoomId;
    std::string Order; // validated spec `order`, empty when absent/invalid
    std::vector<std::string> Via;
    std::int64_t OriginServerTs = 0;
};

// Spec v1.19 §Ordering of children within a space: children with a valid
// `order` first, lexicographically by code-point; the rest by the
// origin_server_ts of their m.space.child event; ties break on ts then room id.
bool CompareSpaceChildren(const SpaceChild &A, const SpaceChild &B);

// Flat, UI-ready view of a room (list row, room header, details screen).
struct RoomSummary {
    std::string RoomId;
    std::string Name;
    std::string Topic;
    std::string AvatarMxc;
    std::string CanonicalAlias;
    MembershipState Membership = MembershipState::None;
    bool IsDirect = false;
    bool IsEncrypted = false;
    bool IsFavourite = false;
    bool IsLowPriority = false;
    bool IsSpace = false;          // m.room.create content type == m.space
    std::int32_t SpaceChildCount = 0; // valid m.space.child edges (spaces only)
    std::int64_t MemberCount = 0;
    std::int32_t NotificationCount = 0;
    std::int32_t HighlightCount = 0;
    std::int64_t LastActivityTs = 0;
    std::string LastMessagePreview;
    std::string LastMessageSender;
    std::string ReadMarkerEventId;

    bool HasUnread() const;
};

// Cheap invalidation summary handed to the UI after each /sync batch.
struct SyncSummary {
    std::vector<std::string> ChangedRoomIds;
    std::vector<std::string> InvitedRoomIds;
    std::vector<std::string> LeftRoomIds;
    bool PresenceChanged = false;
    bool AccountDataChanged = false;
};

// --- Display helpers (shared by the timeline and previews) ---

// Name, falling back to the canonical alias, then the room id.
std::string RoomDisplayName(const RoomSummary &Summary);
bool IsMessageEvent(const Event &Ev);
std::string GetMessageBody(const Event &Ev);
std::string GetFormattedMessageBody(const Event &Ev);
std::string EventDisplayText(const Event &Ev, const std::string &SelfUserId);
std::string EventPreviewText(const Event &Ev, const std::string &SelfUserId);
// "Kind: name (size)" label for image/video/audio/file/sticker messages,
// empty for anything else (including redacted and undecryptable events).
std::string AttachmentMarker(const Event &Ev);
// Previewable image of an image/sticker message: the mxc:// source plus the
// declared pixel size (0 when unknown). For encrypted attachments MxcUrl
// points at the ciphertext and EncryptedFile is the spec's encrypted file
// object (key/iv/hash). Empty MxcUrl when the event has no previewable image.
struct AttachmentImage {
    std::string MxcUrl;
    nlohmann::json EncryptedFile = nlohmann::json::object();
    bool Encrypted = false;
    std::int32_t Width = 0;
    std::int32_t Height = 0;
};
AttachmentImage GetAttachmentImage(const Event &Ev);
std::string GetReplacesEventId(const Event &Ev); // m.replace target, empty when not an edit
std::string GetReplyToEventId(const Event &Ev);  // m.in_reply_to target, empty when not a reply
std::vector<ReactionGroup> CollectReactions(const std::vector<Event> &Timeline, const std::string &SelfUserId);

// Flattens the subset of Matrix HTML used in formatted_body to plain text.
std::string HtmlToPlainText(const std::string &Html);

} // namespace NxMatrix::Matrix