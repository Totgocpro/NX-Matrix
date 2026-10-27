// Per-room in-memory state: applies /sync batches and backfill on the sync
// thread, serves lock-guarded snapshots to the UI thread.
#include "NxMatrix/Matrix/Session.hpp"

#include "NxMatrix/E2ee/E2eeManager.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace NxMatrix::Matrix {

namespace {

constexpr std::int64_t TypingStaleMs = 15000;

// Shared fallback so ternaries bind a reference instead of copying JSON.
const nlohmann::json EmptyJson = nlohmann::json::object();

bool HasStateKey(const nlohmann::json &Raw) {
    return Raw.contains("state_key") && Raw["state_key"].is_string();
}

std::string GetTransactionId(const nlohmann::json &Raw) {
    if(!Raw.contains("unsigned") || !Raw["unsigned"].is_object()) {
        return {};
    }
    return Util::JsonGetString(Raw["unsigned"], "transaction_id");
}

// Leading number of a room_version ("12" -> 12, "org.matrix.msc1234" -> 1).
// Unknown/unstable versions fall back to the oldest rules so we never grant
// the immutable creator level on a guess.
int ParseRoomVersion(const std::string &Text) {
    int Version = 0;
    for(const char Ch : Text) {
        if(Ch < '0' || Ch > '9') {
            break;
        }
        Version = Version * 10 + (Ch - '0');
        if(Version > 1000) {
            break;
        }
    }
    return Version > 0 ? Version : 1;
}

} // namespace

Session::Session(E2ee::E2eeManager &E2ee) : m_E2ee(E2ee) {}

void Session::SetOwnUserId(const std::string &UserId) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_OwnUserId = UserId;
    BumpVersion();
}

const std::string &Session::GetOwnUserId() const {
    // Identity is set at login before the sync thread starts; returning a
    // reference cannot lock, so callers must not race SetOwnUserId.
    return m_OwnUserId;
}

std::uint64_t Session::GetVersion() const {
    return m_Version.load(std::memory_order_acquire);
}

void Session::BumpVersion() {
    m_Version.fetch_add(1, std::memory_order_release);
}

Session::RoomData &Session::EnsureRoom(const std::string &RoomId) {
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        RoomData &Room = m_Rooms[RoomId];
        Room.Summary.RoomId = RoomId;
        return Room;
    }
    return It->second;
}

void Session::ApplyDecryption(Event &Ev) {
    if(Ev.Type != "m.room.encrypted") {
        return;
    }
    nlohmann::json Plain;
    std::string PlainType;
    bool Failed = false;
    if(m_E2ee.DecryptRoomEvent(Ev, Plain, PlainType, Failed)) {
        Ev.Type = PlainType;
        Ev.Content = std::move(Plain);
        Ev.IsDecrypted = true;
    } else {
        // Keep the raw payload so a late key arrival can still decrypt it
        // through SetEventDecryptedContent.
        Ev.IsDecryptionFailed = true;
    }
}

void Session::TouchUserInfoFromMember(const RoomMember &Member) {
    UserInfo &Info = m_Users[Member.UserId];
    Info.UserId = Member.UserId;
    if(!Member.DisplayName.empty()) {
        Info.DisplayName = Member.DisplayName;
    }
    if(!Member.AvatarMxc.empty()) {
        Info.AvatarMxc = Member.AvatarMxc;
    }
}

void Session::ApplyMemberEvent(RoomData &Room, const Event &Ev) {
    const std::string UserId = Ev.StateKey;
    if(UserId.empty()) {
        return;
    }
    RoomMember Member;
    auto It = Room.Members.find(UserId);
    if(It != Room.Members.end()) {
        Member = It->second;
    }
    Member.UserId = UserId;
    const std::string Membership = Util::JsonGetString(Ev.Content, "membership");
    if(!Membership.empty()) {
        Member.Membership = MembershipFromString(Membership);
    }
    const std::string DisplayName = Util::JsonGetString(Ev.Content, "displayname");
    if(!DisplayName.empty()) {
        Member.DisplayName = DisplayName;
    }
    const std::string AvatarMxc = Util::JsonGetString(Ev.Content, "avatar_url");
    if(!AvatarMxc.empty()) {
        Member.AvatarMxc = AvatarMxc;
    }
    Room.Members[UserId] = Member;
    TouchUserInfoFromMember(Member);

    if(UserId == m_OwnUserId && Member.Membership != MembershipState::None) {
        Room.Summary.Membership = Member.Membership;
    }

    std::int64_t Joined = 0;
    for(const auto &Pair : Room.Members) {
        if(Pair.second.Membership == MembershipState::Join) {
            ++Joined;
        }
    }
    Room.Summary.MemberCount = Joined;
}

void Session::ApplySpaceChild(RoomData &Room, const Event &Ev) {
    if(Ev.StateKey.empty()) {
        return;
    }
    // Spec v1.19 §m.space.child: the child only belongs to the space while
    // the content carries a valid `via` list; cleared content (or a
    // redaction) removes the edge.
    std::vector<std::string> Via;
    if(Ev.Content.is_object() && Ev.Content.contains("via") && Ev.Content["via"].is_array()) {
        for(const auto &Entry : Ev.Content["via"]) {
            if(Entry.is_string()) {
                Via.push_back(Entry.get<std::string>());
            }
        }
    }
    if(Via.empty()) {
        Room.SpaceChildren.erase(Ev.StateKey);
    } else {
        SpaceChild Child;
        Child.RoomId = Ev.StateKey;
        Child.Via = std::move(Via);
        Child.OriginServerTs = Ev.Timestamp;
        // `order`: ASCII \x20-\x7E, at most 50 chars; anything else counts
        // as absent (spec v1.19 §m.space.child).
        const std::string Order = Util::JsonGetString(Ev.Content, "order");
        if(!Order.empty() && Order.size() <= 50 &&
           std::all_of(Order.begin(), Order.end(), [](char Ch) { return Ch >= '\x20' && Ch <= '\x7E'; })) {
            Child.Order = Order;
        }
        Room.SpaceChildren[Ev.StateKey] = std::move(Child);
    }
    Room.Summary.SpaceChildCount = static_cast<std::int32_t>(Room.SpaceChildren.size());
}

void Session::ApplyStateToRoom(RoomData &Room, const Event &Ev) {
    // m.space.child has one event per child room (state key = child id): it
    // lives in the dedicated map, never in the type -> content State map.
    if(Ev.Type == "m.space.child") {
        ApplySpaceChild(Room, Ev);
        return;
    }
    Room.State[Ev.Type] = Ev.Content;

    if(Ev.Type == "m.room.member") {
        ApplyMemberEvent(Room, Ev);
    } else if(Ev.Type == "m.room.create") {
        // Creator identity lives here, not in m.room.power_levels (absent in
        // room v12+): content.creator for v1-10, sender from v11 on.
        std::string Creator = Util::JsonGetString(Ev.Content, "creator");
        Room.CreatorUserId = Creator.empty() ? Ev.Sender : Creator;
        Room.RoomVersion = ParseRoomVersion(Util::JsonGetString(Ev.Content, "room_version"));
        // Spec v1.19 §Spaces: a space is a room with type m.space.
        Room.Summary.IsSpace = Util::JsonGetString(Ev.Content, "type") == "m.space";
    } else if(Ev.Type == "m.room.power_levels") {
        Room.HasPowerLevels = true;
        Room.Power = PowerLevels::FromJson(Ev.Content);
    } else if(Ev.Type == "m.room.name") {
        Room.Summary.Name = Util::JsonGetString(Ev.Content, "name");
    } else if(Ev.Type == "m.room.topic") {
        Room.Summary.Topic = Util::JsonGetString(Ev.Content, "topic");
    } else if(Ev.Type == "m.room.avatar") {
        // Spec: m.room.avatar state event, content key avatar_url.
        Room.Summary.AvatarMxc = Util::JsonGetString(Ev.Content, "avatar_url");
    } else if(Ev.Type == "m.room.canonical_alias") {
        Room.Summary.CanonicalAlias = Util::JsonGetString(Ev.Content, "alias");
    } else if(Ev.Type == "m.room.encryption") {
        Room.Summary.IsEncrypted = !Util::JsonGetString(Ev.Content, "algorithm").empty();
    }
    // m.room.join_rules and friends only live in the State map (the fixed
    // RoomSummary has no field for them).
}

void Session::ApplyTimelineEvent(RoomData &Room, const nlohmann::json &Raw) {
    if(!Raw.is_object()) {
        return;
    }
    // Sync timeline events carry no room_id; the enclosing block does.
    Event Ev = Event::FromJson(Raw, Room.Summary.RoomId);
    const std::string TxnId = GetTransactionId(Raw);

    // (a) Local-echo dedupe: a server echo carrying the transaction id of an
    // event we already show (pending or resolved) replaces it in place.
    std::size_t ReplaceAt = static_cast<std::size_t>(-1);
    if(!TxnId.empty()) {
        for(std::size_t Index = 0; Index < Room.Timeline.size(); ++Index) {
            if(!Room.Timeline[Index].TransactionId.empty() && Room.Timeline[Index].TransactionId == TxnId) {
                ReplaceAt = Index;
                break;
            }
        }
    }

    // (d) E2EE: decrypt before any display/summary consumer sees the event.
    ApplyDecryption(Ev);

    // (b) Redaction: mark the already-known target, keep the redaction event
    // itself in the stream.
    if(Ev.Type == "m.room.redaction") {
        std::string RedactsId = Util::JsonGetString(Raw, "redacts");
        if(RedactsId.empty()) {
            RedactsId = Util::JsonGetString(Raw.contains("content") ? Raw["content"] : EmptyJson, "redacts");
        }
        for(auto &Existing : Room.Timeline) {
            if(!RedactsId.empty() && Existing.EventId == RedactsId && !Existing.IsRedacted) {
                Existing.IsRedacted = true;
                Existing.Content = nlohmann::json::object();
                break;
            }
        }
    }

    // (c) m.replace edit: merge the new content into the target so the row
    // renders the latest text; the edit row itself stays for UI filtering.
    const std::string ReplacesId = GetReplacesEventId(Ev);
    if(!ReplacesId.empty()) {
        for(auto &Existing : Room.Timeline) {
            if(Existing.EventId == ReplacesId) {
                if(Ev.Content.contains("m.new_content") && Ev.Content["m.new_content"].is_object()) {
                    for(auto It = Ev.Content["m.new_content"].begin(); It != Ev.Content["m.new_content"].end(); ++It) {
                        Existing.Content[It.key()] = It.value();
                    }
                }
                if(!Ev.EventId.empty()) {
                    Existing.Unsigned["replaced_by"] = Ev.EventId;
                }
                break;
            }
        }
    }

    const bool IsState = HasStateKey(Raw);

    // Overlapping batches (/sync with limited gaps, /messages backfill) can
    // re-send events we already show: locate the existing row before appending.
    std::size_t ExistingAt = static_cast<std::size_t>(-1);
    if(ReplaceAt == static_cast<std::size_t>(-1) && !Ev.EventId.empty()) {
        for(std::size_t Index = 0; Index < Room.Timeline.size(); ++Index) {
            if(Room.Timeline[Index].EventId == Ev.EventId) {
                ExistingAt = Index;
                break;
            }
        }
    }

    if(ReplaceAt != static_cast<std::size_t>(-1)) {
        Ev.TransactionId = TxnId; // keep the link for later resolve/remove
        Ev.IsLocalEcho = false;
        Room.Timeline[ReplaceAt] = std::move(Ev);
    } else if(ExistingAt != static_cast<std::size_t>(-1)) {
        // Known event: never duplicate the row, but keep a late redaction of
        // an already-known event visible.
        if(Ev.IsRedacted && !Room.Timeline[ExistingAt].IsRedacted) {
            Room.Timeline[ExistingAt].IsRedacted = true;
            Room.Timeline[ExistingAt].Content = nlohmann::json::object();
        }
        return;
    } else {
        Room.Timeline.emplace_back(std::move(Ev));
        ReplaceAt = Room.Timeline.size() - 1;
    }
    Event &Stored = Room.Timeline[ReplaceAt];
    if(IsState) {
        ApplyStateToRoom(Room, Stored);
    }

    // (e) preview: newest non-state, non-reaction message wins.
    if(!IsState && Stored.Type != "m.reaction" && IsMessageEvent(Stored)) {
        Room.Summary.LastMessagePreview = EventPreviewText(Stored, m_OwnUserId);
        Room.Summary.LastMessageSender = Stored.Sender;
        Room.Summary.LastActivityTs = Stored.Timestamp;
    }
}

// --- Writes ---

void Session::ApplySync(const nlohmann::json &Sync, SyncSummary &OutSummary) {
    if(!Sync.is_object()) {
        return;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);

    const auto &Rooms = Sync.contains("rooms") && Sync["rooms"].is_object() ? Sync["rooms"] : EmptyJson;

    // Invites first so a room present in both blocks ends up as Join.
    std::set<std::string> Changed;
    if(Rooms.contains("invite") && Rooms["invite"].is_object()) {
        for(auto It = Rooms["invite"].begin(); It != Rooms["invite"].end(); ++It) {
            const std::string &RoomId = It.key();
            const auto &Block = It.value();
            if(!Block.is_object()) {
                continue;
            }
            RoomData &Room = EnsureRoom(RoomId);
            if(Room.Summary.Membership != MembershipState::Join) {
                Room.Summary.Membership = MembershipState::Invite;
            }
            const auto &InviteState =
                Block.contains("invite_state") && Block["invite_state"].is_object() ? Block["invite_state"] : EmptyJson;
            if(!InviteState.contains("events") || !InviteState["events"].is_array()) {
                continue;
            }
            // Minimal room: name/topic only, no timeline.
            for(const auto &Raw : InviteState["events"]) {
                if(!Raw.is_object()) {
                    continue;
                }
                const auto &Content = Raw.contains("content") && Raw["content"].is_object() ? Raw["content"] : EmptyJson;
                const std::string Type = Util::JsonGetString(Raw, "type");
                if(Type == "m.room.name") {
                    Room.Summary.Name = Util::JsonGetString(Content, "name");
                } else if(Type == "m.room.topic") {
                    Room.Summary.Topic = Util::JsonGetString(Content, "topic");
                } else if(Type == "m.room.avatar") {
                    // Stripped invite state carries the room icon too.
                    Room.Summary.AvatarMxc = Util::JsonGetString(Content, "avatar_url");
                } else if(Type == "m.room.create") {
                    // Stripped invite state carries the room type: a space
                    // invitation must be identifiable before it is accepted.
                    Room.Summary.IsSpace = Util::JsonGetString(Content, "type") == "m.space";
                }
            }
            OutSummary.InvitedRoomIds.push_back(RoomId);
            Changed.insert(RoomId);
        }
    }

    if(Rooms.contains("join") && Rooms["join"].is_object()) {
        for(auto It = Rooms["join"].begin(); It != Rooms["join"].end(); ++It) {
            const std::string &RoomId = It.key();
            const auto &Block = It.value();
            if(!Block.is_object()) {
                continue;
            }
            RoomData &Room = EnsureRoom(RoomId);
            Room.Summary.Membership = MembershipState::Join;
            Changed.insert(RoomId);

            // State at the start of the timeline window.
            if(Block.contains("state") && Block["state"].is_object()) {
                const auto &StateBlock = Block["state"];
                if(StateBlock.contains("events") && StateBlock["events"].is_array()) {
                    for(const auto &Raw : StateBlock["events"]) {
                        if(!Raw.is_object()) {
                            continue;
                        }
                        Event Ev = Event::FromJson(Raw, RoomId);
                        ApplyStateToRoom(Room, Ev);
                    }
                }
            }

            // Timeline: local echoes, redactions, edits, decryption, preview.
            if(Block.contains("timeline") && Block["timeline"].is_object()) {
                const auto &TimelineBlock = Block["timeline"];
                if(TimelineBlock.contains("events") && TimelineBlock["events"].is_array()) {
                    for(const auto &Raw : TimelineBlock["events"]) {
                        ApplyTimelineEvent(Room, Raw);
                    }
                }
                const std::string PrevBatch = Util::JsonGetString(TimelineBlock, "prev_batch");
                if(!PrevBatch.empty()) {
                    Room.PrevBatch = PrevBatch;
                }
            }

            // Typing replaces the whole seen-set; fresh timestamps restart
            // the 15s staleness filter in GetTypingUsers.
            if(Block.contains("ephemeral") && Block["ephemeral"].is_object()) {
                const auto &Ephemeral = Block["ephemeral"];
                if(Ephemeral.contains("events") && Ephemeral["events"].is_array()) {
                    for(const auto &Raw : Ephemeral["events"]) {
                        if(!Raw.is_object() || Util::JsonGetString(Raw, "type") != "m.typing") {
                            continue;
                        }
                        const auto &Content = Raw.contains("content") && Raw["content"].is_object() ? Raw["content"] : EmptyJson;
                        std::set<std::string> TypingNow;
                        if(Content.contains("user_ids") && Content["user_ids"].is_array()) {
                            for(const auto &Id : Content["user_ids"]) {
                                if(Id.is_string()) {
                                    TypingNow.insert(Id.get<std::string>());
                                }
                            }
                        }
                        for(auto TypeIt = Room.Typing.begin(); TypeIt != Room.Typing.end();) {
                            TypeIt = TypingNow.count(TypeIt->first) ? std::next(TypeIt) : Room.Typing.erase(TypeIt);
                        }
                        const std::int64_t Now = Util::NowMs();
                        for(const auto &Id : TypingNow) {
                            Room.Typing[Id] = Now;
                        }
                    }
                }
            }

            if(Block.contains("unread_notifications") && Block["unread_notifications"].is_object()) {
                const auto &Unread = Block["unread_notifications"];
                if(Unread.contains("notification_count") && Unread["notification_count"].is_number()) {
                    Room.Summary.NotificationCount = static_cast<std::int32_t>(Util::JsonGetInt(Unread, "notification_count"));
                }
                if(Unread.contains("highlight_count") && Unread["highlight_count"].is_number()) {
                    Room.Summary.HighlightCount = static_cast<std::int32_t>(Util::JsonGetInt(Unread, "highlight_count"));
                }
            }

            if(Block.contains("account_data") && Block["account_data"].is_object()) {
                const auto &Data = Block["account_data"];
                if(Data.contains("events") && Data["events"].is_array()) {
                    for(const auto &Raw : Data["events"]) {
                        if(!Raw.is_object()) {
                            continue;
                        }
                        const auto &Content = Raw.contains("content") && Raw["content"].is_object() ? Raw["content"] : EmptyJson;
                        const std::string Type = Util::JsonGetString(Raw, "type");
                        if(Type == "m.tag") {
                            const auto &Tags = Content.contains("tags") && Content["tags"].is_object() ? Content["tags"] : Content;
                            Room.Summary.IsFavourite = Tags.contains("m.favourite");
                            Room.Summary.IsLowPriority = Tags.contains("m.lowpriority");
                        } else if(Type == "m.fully_read") {
                            // Only feeder for ReadMarkerEventId in the whole app.
                            Room.Summary.ReadMarkerEventId = Util::JsonGetString(Content, "event_id");
                        }
                    }
                }
            }
        }
    }

    if(Rooms.contains("leave") && Rooms["leave"].is_object()) {
        for(auto It = Rooms["leave"].begin(); It != Rooms["leave"].end(); ++It) {
            if(!It.value().is_object()) {
                continue;
            }
            m_Rooms.erase(It.key());
            OutSummary.LeftRoomIds.push_back(It.key());
        }
    }

    for(const auto &RoomId : Changed) {
        OutSummary.ChangedRoomIds.push_back(RoomId);
    }

    if(Sync.contains("presence") && Sync["presence"].is_object()) {
        const auto &Presence = Sync["presence"];
        if(Presence.contains("events") && Presence["events"].is_array() && !Presence["events"].empty()) {
            OutSummary.PresenceChanged = true;
            for(const auto &Raw : Presence["events"]) {
                if(!Raw.is_object()) {
                    continue;
                }
                const std::string UserId = Util::JsonGetString(Raw, "sender");
                if(UserId.empty()) {
                    continue;
                }
                UserInfo &Info = m_Users[UserId];
                Info.UserId = UserId;
                const auto &Content = Raw.contains("content") && Raw["content"].is_object() ? Raw["content"] : EmptyJson;
                Info.Presence = Util::JsonGetString(Content, "presence", Info.Presence);
                if(Content.contains("last_active_ago") && Content["last_active_ago"].is_number()) {
                    Info.LastActiveAgoMs = Util::JsonGetInt(Content, "last_active_ago", Info.LastActiveAgoMs);
                }
            }
        }
    }

    if(Sync.contains("account_data") && Sync["account_data"].is_object()) {
        const auto &Data = Sync["account_data"];
        if(Data.contains("events") && Data["events"].is_array() && !Data["events"].empty()) {
            OutSummary.AccountDataChanged = true;
        }
    }

    BumpVersion();
}

void Session::ApplyHistory(const std::string &RoomId, const std::vector<Event> &OlderEvents, const std::string &NewPrevBatch) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    RoomData &Room = EnsureRoom(RoomId);
    if(Room.Summary.Membership == MembershipState::None) {
        // History is only fetched for a room the user opened; it must already
        // be joined by the time the UI asks for backfill.
        Room.Summary.Membership = MembershipState::Join;
    }
    Room.PrevBatch = NewPrevBatch;

    std::set<std::string> Known;
    for(const auto &Existing : Room.Timeline) {
        if(!Existing.EventId.empty()) {
            Known.insert(Existing.EventId);
        }
    }
    std::vector<Event> Head;
    Head.reserve(OlderEvents.size());
    for(Event Ev : OlderEvents) {
        if(!Ev.EventId.empty() && Known.count(Ev.EventId)) {
            continue;
        }
        if(!Ev.EventId.empty()) {
            Known.insert(Ev.EventId);
        }
        ApplyDecryption(Ev);
        Head.push_back(std::move(Ev));
    }
    Room.Timeline.insert(Room.Timeline.begin(), std::make_move_iterator(Head.begin()), std::make_move_iterator(Head.end()));
    BumpVersion();
}

void Session::UpsertLocalEcho(const Event &Echo) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(Echo.RoomId.empty()) {
        return;
    }
    RoomData &Room = EnsureRoom(Echo.RoomId);
    Event Stored = Echo;
    Stored.IsLocalEcho = true;
    Stored.Timestamp = Util::NowMs();
    for(auto &Existing : Room.Timeline) {
        if(!Stored.TransactionId.empty() && Existing.TransactionId == Stored.TransactionId) {
            Existing = std::move(Stored);
            BumpVersion();
            return;
        }
    }
    Room.Timeline.push_back(std::move(Stored));
    BumpVersion();
}

void Session::ResolveLocalEcho(const std::string &RoomId, const std::string &TransactionId, const std::string &EventId) {
    if(TransactionId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto RoomIt = m_Rooms.find(RoomId);
    if(RoomIt == m_Rooms.end()) {
        return;
    }
    for(auto &Ev : RoomIt->second.Timeline) {
        if(Ev.TransactionId == TransactionId) {
            Ev.EventId = EventId;
            Ev.IsLocalEcho = false;
            break;
        }
    }
    BumpVersion();
}

void Session::RemoveLocalEcho(const std::string &RoomId, const std::string &TransactionId) {
    if(TransactionId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto RoomIt = m_Rooms.find(RoomId);
    if(RoomIt == m_Rooms.end()) {
        return;
    }
    auto &Timeline = RoomIt->second.Timeline;
    Timeline.erase(
        std::remove_if(Timeline.begin(), Timeline.end(), [&TransactionId](const Event &Ev) { return Ev.TransactionId == TransactionId; }),
        Timeline.end());
    BumpVersion();
}

void Session::SetTypingUsers(const std::string &RoomId, const std::vector<std::string> &UserIds) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto RoomIt = m_Rooms.find(RoomId);
    if(RoomIt == m_Rooms.end()) {
        return; // room unknown until the first sync delivers it
    }
    auto &Typing = RoomIt->second.Typing;
    Typing.clear();
    const std::int64_t Now = Util::NowMs();
    for(const auto &UserId : UserIds) {
        Typing[UserId] = Now;
    }
    BumpVersion();
}

void Session::SetEventDecryptedContent(const std::string &RoomId, const std::string &EventId, const std::string &EventType,
                                       const nlohmann::json &Content, bool Failed) {
    if(EventId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto RoomIt = m_Rooms.find(RoomId);
    if(RoomIt == m_Rooms.end()) {
        return;
    }
    for(auto &Ev : RoomIt->second.Timeline) {
        if(Ev.EventId == EventId) {
            Ev.Type = EventType;
            Ev.Content = Content;
            Ev.IsDecrypted = !Failed;
            Ev.IsDecryptionFailed = Failed;
            break;
        }
    }
    BumpVersion();
}

// --- Reads (UI thread) ---

std::vector<RoomSummary> Session::GetRoomSummaries() const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    std::vector<RoomSummary> Out;
    Out.reserve(m_Rooms.size());
    for(const auto &Pair : m_Rooms) {
        Out.push_back(Pair.second.Summary);
    }
    // Favourites pin to the top; IsLowPriority is left for the UI to group.
    std::stable_sort(Out.begin(), Out.end(), [](const RoomSummary &A, const RoomSummary &B) {
        if(A.IsFavourite != B.IsFavourite) {
            return A.IsFavourite;
        }
        return A.LastActivityTs > B.LastActivityTs;
    });
    return Out;
}

bool Session::GetRoomSummary(const std::string &RoomId, RoomSummary &Out) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return false;
    }
    Out = It->second.Summary;
    return true;
}

std::vector<Event> Session::GetTimeline(const std::string &RoomId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return {};
    }
    return It->second.Timeline;
}

std::vector<RoomMember> Session::GetMembers(const std::string &RoomId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    std::vector<RoomMember> Out;
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return Out;
    }
    Out.reserve(It->second.Members.size());
    for(const auto &Pair : It->second.Members) {
        RoomMember Member = Pair.second;
        if(Member.DisplayName.empty()) {
            Member.DisplayName = Member.UserId;
        }
        Out.push_back(std::move(Member));
    }
    return Out;
}

bool Session::GetMember(const std::string &RoomId, const std::string &UserId, RoomMember &Out) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto RoomIt = m_Rooms.find(RoomId);
    if(RoomIt == m_Rooms.end()) {
        return false;
    }
    auto It = RoomIt->second.Members.find(UserId);
    if(It == RoomIt->second.Members.end()) {
        return false;
    }
    Out = It->second;
    if(Out.DisplayName.empty()) {
        Out.DisplayName = Out.UserId;
    }
    return true;
}

PowerLevels Session::PowerOf(const RoomData &Room) {
    PowerLevels Out = Room.Power;
    Out.CreatorUserId = Room.CreatorUserId;
    Out.RoomVersion = Room.RoomVersion;
    Out.HasPowerLevelsEvent = Room.HasPowerLevels;
    return Out;
}

PowerLevels Session::GetPowerLevels(const std::string &RoomId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return {};
    }
    return PowerOf(It->second);
}

bool Session::CanSendState(const std::string &RoomId, const std::string &EventType) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return false;
    }
    const RoomData &Room = It->second;
    // Membership is checked before levels: only joined members may send state.
    if(Room.Summary.Membership != MembershipState::Join) {
        return false;
    }
    const PowerLevels Levels = PowerOf(Room);
    return Levels.CanSendState(EventType, Levels.GetUserLevel(m_OwnUserId));
}

std::vector<std::string> Session::GetTypingUsers(const std::string &RoomId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    std::vector<std::string> Out;
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return Out;
    }
    const std::int64_t Now = Util::NowMs();
    for(const auto &Pair : It->second.Typing) {
        if(Now - Pair.second <= TypingStaleMs) {
            Out.push_back(Pair.first);
        }
    }
    return Out;
}

bool Session::GetUserInfo(const std::string &UserId, UserInfo &Out) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Users.find(UserId);
    if(It == m_Users.end()) {
        return false;
    }
    Out = It->second;
    return true;
}

bool Session::IsRoomEncrypted(const std::string &RoomId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Rooms.find(RoomId);
    return It != m_Rooms.end() && It->second.Summary.IsEncrypted;
}

bool Session::HasRoom(const std::string &RoomId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    return m_Rooms.count(RoomId) != 0;
}

void Session::ApplyFetchedState(const std::string &RoomId, const std::string &EventType, const nlohmann::json &Content,
                                const std::string &StateKey, std::int64_t Timestamp) {
    if(!Content.is_object()) {
        return;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);
    auto It = m_Rooms.find(RoomId);
    if(It == m_Rooms.end()) {
        return;
    }
    Event Ev;
    Ev.RoomId = RoomId;
    Ev.Type = EventType;
    Ev.Content = Content;
    Ev.StateKey = StateKey;
    Ev.Timestamp = Timestamp;
    ApplyStateToRoom(It->second, Ev);
    BumpVersion();
}

std::vector<SpaceChild> Session::GetSpaceChildren(const std::string &SpaceId) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    std::vector<SpaceChild> Out;
    auto It = m_Rooms.find(SpaceId);
    if(It == m_Rooms.end()) {
        return Out;
    }
    Out.reserve(It->second.SpaceChildren.size());
    for(const auto &Pair : It->second.SpaceChildren) {
        Out.push_back(Pair.second);
    }
    std::sort(Out.begin(), Out.end(), CompareSpaceChildren);
    return Out;
}

std::vector<std::string> Session::GetRoomIdsMissingState(const std::string &EventType) const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    std::vector<std::string> Out;
    for(const auto &Pair : m_Rooms) {
        if(Pair.second.Summary.Membership == MembershipState::Join && Pair.second.State.count(EventType) == 0) {
            Out.push_back(Pair.first);
        }
    }
    return Out;
}

} // namespace NxMatrix::Matrix