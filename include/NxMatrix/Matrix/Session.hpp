#pragma once
#include "NxMatrix/Matrix/Models.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace NxMatrix::E2ee {
class E2eeManager;
}

namespace NxMatrix::Matrix {

// In-memory room/user state shared by the sync thread and the UI.
// Writes come from SyncEngine (sync thread) and send callbacks (worker
// threads); reads are lock-guarded snapshots safe for the UI thread.
// E2EE events are decrypted during ApplySync/ApplyHistory, i.e. off the UI
// thread, so rendering never pays the crypto cost.
class Session {
public:
    explicit Session(E2ee::E2eeManager &E2ee);

    void SetOwnUserId(const std::string &UserId);
    const std::string &GetOwnUserId() const;

    // Monotonic counter bumped on every write: the UI polls it per frame to
    // know when a screen must refresh.
    std::uint64_t GetVersion() const;

    // --- Writes ---
    void ApplySync(const nlohmann::json &Sync, SyncSummary &OutSummary);
    void ApplyHistory(const std::string &RoomId, const std::vector<Event> &OlderEvents, const std::string &NewPrevBatch);
    void UpsertLocalEcho(const Event &Echo);
    void ResolveLocalEcho(const std::string &RoomId, const std::string &TransactionId, const std::string &EventId);
    void RemoveLocalEcho(const std::string &RoomId, const std::string &TransactionId);
    void SetTypingUsers(const std::string &RoomId, const std::vector<std::string> &UserIds);
    void SetEventDecryptedContent(const std::string &RoomId, const std::string &EventId, const std::string &EventType,
                                  const nlohmann::json &Content, bool Failed);
    // Applies a state event fetched outside /sync (avatar backfill) or sent
    // optimistically (m.space.child add/remove) exactly like a sync state
    // block would, then bumps the version.
    void ApplyFetchedState(const std::string &RoomId, const std::string &EventType, const nlohmann::json &Content,
                           const std::string &StateKey = {}, std::int64_t Timestamp = 0);
    // Room-list snapshot persisted by the app layer: summaries and space
    // children, so the list can paint before /sync answers. Timelines stay
    // out of it and are paginated on room open.
    nlohmann::json ExportCache() const;
    void ImportCache(const nlohmann::json &Cache);

    // --- Reads (UI thread) ---
    std::vector<RoomSummary> GetRoomSummaries() const;
    bool GetRoomSummary(const std::string &RoomId, RoomSummary &Out) const;
    std::vector<Event> GetTimeline(const std::string &RoomId) const;
    std::vector<RoomMember> GetMembers(const std::string &RoomId) const;
    bool GetMember(const std::string &RoomId, const std::string &UserId, RoomMember &Out) const;
    PowerLevels GetPowerLevels(const std::string &RoomId) const;
    // Local pre-check for sending a state event: membership first, then the
    // power levels (spec v1.19 §Permissions). The server stays authoritative.
    bool CanSendState(const std::string &RoomId, const std::string &EventType) const;
    std::vector<std::string> GetTypingUsers(const std::string &RoomId) const;
    // Valid m.space.child children of a space-room, in spec order
    // (v1.19 §Ordering of children within a space).
    std::vector<SpaceChild> GetSpaceChildren(const std::string &SpaceId) const;
    bool GetUserInfo(const std::string &UserId, UserInfo &Out) const;
    bool IsRoomEncrypted(const std::string &RoomId) const;
    bool HasRoom(const std::string &RoomId) const;
    // Joined rooms that never reported this state event type: an account
    // resumed from a stored sync token only receives changes, so events that
    // predate the token (e.g. m.room.avatar) never arrive on their own.
    std::vector<std::string> GetRoomIdsMissingState(const std::string &EventType) const;

private:
    // Full per-room state. Every field is touched under m_Mutex.
    struct RoomData {
        RoomSummary Summary;
        std::vector<Event> Timeline;
        std::map<std::string, nlohmann::json> State; // state event type -> content
        std::map<std::string, RoomMember> Members;   // user id -> member
        PowerLevels Power;
        std::string CreatorUserId; // m.room.create: content.creator, else sender
        int RoomVersion = 1;       // m.room.create: room_version, defaults to 1
        bool HasPowerLevels = false;
        std::string PrevBatch;
        std::map<std::string, std::int64_t> Typing; // user id -> last typing seen (ms)
        // m.space.child edges keyed by child room id (many state keys of one
        // type, so they cannot live in the type -> content State map).
        std::map<std::string, SpaceChild> SpaceChildren;
    };

    // The helpers below assume the caller already holds m_Mutex.
    RoomData &EnsureRoom(const std::string &RoomId);
    static PowerLevels PowerOf(const RoomData &Room); // Power + creator/version context
    void ApplyStateToRoom(RoomData &Room, const Event &Ev);
    static void ApplySpaceChild(RoomData &Room, const Event &Ev);
    void ApplyTimelineEvent(RoomData &Room, const nlohmann::json &Raw);
    void ApplyDecryption(Event &Ev);
    void ApplyMemberEvent(RoomData &Room, const Event &Ev);
    void TouchUserInfoFromMember(const RoomMember &Member);
    void BumpVersion();

    mutable std::mutex m_Mutex;
    std::atomic<std::uint64_t> m_Version{0};
    std::string m_OwnUserId;
    E2ee::E2eeManager &m_E2ee;
    std::map<std::string, RoomData> m_Rooms;
    std::map<std::string, UserInfo> m_Users;
};

} // namespace NxMatrix::Matrix