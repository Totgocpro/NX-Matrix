// Room timeline: renders the loaded window of the session timeline (day and
// state headers, message bubbles with replies/reactions), composer over the
// Horizon keyboard, read markers, back-pagination and per-message actions.
#include "NxMatrix/Ui/Layouts/RoomLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Crypto/Random.hpp"
#include "NxMatrix/Store/SecureStore.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/ReactionPickerLayout.hpp"
#include "NxMatrix/Ui/Layouts/RoomDetailsLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <algorithm>
#include <cstdio>
#include <sys/stat.h>
#include <unordered_map>

namespace NxMatrix::Ui {

namespace {

// The composer sits at the very bottom of the list viewport.
constexpr std::int32_t BottomMargin = 16;
constexpr std::int32_t ComposerHeight = Theme::RowHeightCompact;
constexpr std::size_t InitialWindow = 150;
constexpr const char *ComposerHint = "A Send  B Back  Y Details  X Bottom";

std::string RandomTxn() {
    static const char *HexDigits = "0123456789abcdef";
    const auto Bytes = Crypto::RandomBytes(8);
    std::string Out;
    Out.reserve(Bytes.size() * 2);
    for(const auto Byte : Bytes) {
        Out.push_back(HexDigits[(Byte >> 4) & 0x0F]);
        Out.push_back(HexDigits[Byte & 0x0F]);
    }
    return Out;
}

// Reads the m.relates_to annotation info of a reaction event.
bool ReactionTarget(const Matrix::Event &Event, std::string &OutEventId, std::string &OutKey) {
    if(Event.Type != "m.reaction" || !Event.Content.contains("m.relates_to")) {
        return false;
    }
    const auto &Relates = Event.Content["m.relates_to"];
    if(!Relates.contains("event_id") || !Relates["event_id"].is_string()) {
        return false;
    }
    if(Relates.contains("rel_type") && Relates["rel_type"].is_string() && Relates["rel_type"].get<std::string>() != "m.annotation") {
        return false;
    }
    OutEventId = Relates["event_id"].get<std::string>();
    if(Relates.contains("key") && Relates["key"].is_string()) {
        OutKey = Relates["key"].get<std::string>();
    }
    return true;
}

// Local echoes render with a trailing " ..." until the server confirms them.
Matrix::Event WithEchoSuffix(Matrix::Event Event) {
    if(!Event.IsLocalEcho) {
        return Event;
    }
    const std::string Body = Matrix::GetMessageBody(Event);
    Event.Content["body"] = Body + " ...";
    if(Event.Content.contains("formatted_body") && Event.Content["formatted_body"].is_string()) {
        Event.Content["formatted_body"] = Matrix::HtmlToPlainText(Event.Content["formatted_body"].get<std::string>()) + " ...";
    }
    return Event;
}

std::string SafeFileName(const std::string &Candidate) {
    std::string Out;
    Out.reserve(Candidate.size());
    for(const char Ch : Candidate) {
        const bool Keep =
            (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '.' || Ch == '-' || Ch == '_';
        Out.push_back(Keep ? Ch : '_');
    }
    const auto FirstGood = Out.find_first_not_of('.');
    if(FirstGood == std::string::npos) {
        return "attachment";
    }
    Out = FirstGood > 0 ? Out.substr(FirstGood) : Out;
    if(Out.size() > 100) {
        Out = Out.substr(Out.size() - 100);
    }
    return Out;
}

bool SaveDownload(const std::string &Name, const ByteArray &Data) {
    const std::string Dir = Store::SecureStore::GetRootPath() + "/Downloads";
    // Existing directories make mkdir fail; the fopen below is the real check.
    ::mkdir(Store::SecureStore::GetRootPath().c_str(), 0777);
    ::mkdir(Dir.c_str(), 0777);
    const std::string Path = Dir + "/" + Name;
    std::FILE *File = std::fopen(Path.c_str(), "wb");
    if(File == nullptr) {
        return false;
    }
    const bool Written = Data.empty() || std::fwrite(Data.data(), 1, Data.size(), File) == Data.size();
    std::fclose(File);
    if(!Written) {
        std::remove(Path.c_str());
    }
    return Written;
}

// The rounded composer bar (scrolls with the list as its last row); the text
// itself is the layout's m_Composer TextBlock drawn as an overlay.
class ComposerRow : public Row {
public:
    ComposerRow(std::int32_t Width, std::int32_t Height, const std::string &Hint) : Row(Width, Height) {
        const auto HintTexture = pu::ui::render::RenderText(Theme::FontTiny, Hint, Theme::TextFaint);
        m_HintTex = pu::sdl2::TextureHandle::New(HintTexture);
        if(HintTexture != nullptr) {
            pu::ui::render::GetTextDimensions(Theme::FontTiny, Hint, m_HintW, m_HintH);
        }
    }

    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override {
        Drawer->RenderRoundedRectangleFill(Theme::Surface, X, Y, m_Width, m_Height, Theme::CornerRadius);
        Drawer->RenderRoundedRectangle(m_Focused ? Theme::AccentDim : Theme::Border, X, Y, m_Width, m_Height, Theme::CornerRadius);
        if(m_HintTex->Get() != nullptr) {
            Drawer->RenderTexture(m_HintTex->Get(), X + m_Width - 20 - m_HintW, Y + (m_Height - m_HintH) / 2);
        }
    }

private:
    pu::sdl2::TextureHandle::Ref m_HintTex;
    std::int32_t m_HintW = 0;
    std::int32_t m_HintH = 0;
};

} // namespace

RoomLayout::RoomLayout(std::shared_ptr<AppContext> Context, const std::string &RoomId) : m_Context(std::move(Context)), m_RoomId(RoomId) {
    this->SetBackgroundColor(Theme::Background);

    m_Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 24, "");
    m_Header->SetFont(Theme::FontHeading);
    m_Header->SetColor(Theme::Accent);
    this->Add(m_Header);

    m_Meta = pu::ui::elm::TextBlock::New(Theme::ScreenWidth - Theme::ScreenMargin, 34, "");
    m_Meta->SetFont(Theme::FontSmall);
    m_Meta->SetColor(Theme::TextSecondary);
    m_Meta->SetVisible(false);
    this->Add(m_Meta);

    m_Typing = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 84, "");
    m_Typing->SetFont(Theme::FontTiny);
    m_Typing->SetColor(Theme::Accent);
    m_Typing->SetVisible(false);
    this->Add(m_Typing);

    m_StatusLine = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 114, "");
    m_StatusLine->SetFont(Theme::FontTiny);
    m_StatusLine->SetColor(Theme::TextSecondary);
    m_StatusLine->SetVisible(false);
    this->Add(m_StatusLine);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - BottomMargin);
    this->Add(m_List);

    m_Composer = pu::ui::elm::TextBlock::New(Theme::ScreenMargin + 20, Theme::ScreenHeight - BottomMargin - ComposerHeight + 16, "Message");
    m_Composer->SetFont(Theme::FontBody);
    m_Composer->SetColor(Theme::TextFaint);
    const auto HintWidth = pu::ui::render::GetTextWidth(Theme::FontTiny, ComposerHint);
    m_Composer->SetClampWidth(Theme::ScreenWidth - 2 * Theme::ScreenMargin - 20 - HintWidth - 60);
    this->Add(m_Composer);

    GetList()->SetOnActivate([this](std::size_t Index) {
        if(Index < m_RowEventIds.size() && !m_RowEventIds[Index].empty()) {
            OnMessageActivated(Index);
            return;
        }
        // The composer bar is the last row.
        if(Index + 1 == GetList()->GetRowCount()) {
            OnComposerActivated();
        }
    });

    GetList()->SetOnSelectionChanged([this](std::size_t Index) {
        const auto Count = GetList()->GetRowCount();
        if(Count == 0) {
            return;
        }
        m_StickToBottom = (Index + 2 >= Count);
        if(Index + 3 >= Count) {
            SendReadMarker();
        }
        if(Index == 0) {
            LoadOlder();
        }
    });

    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        if(KeysDown & HidNpadButton_Y) {
            m_Context->Nav.Push(RoomDetailsLayout::New(m_Context, m_RoomId));
        } else if(KeysDown & HidNpadButton_X) {
            m_StickToBottom = true;
            GetList()->ScrollToBottom();
        }
    });

    // Rows follow the session version + a cheap window fingerprint; header,
    // typing and composer chrome update every frame but only re-render on change.
    this->AddRenderCallback([this]() {
        const auto Version = m_Context->State.GetVersion();
        if(m_Dirty || Version != m_LastVersion) {
            m_LastVersion = Version;
            if(m_Dirty || ComputeSignature() != m_LastSignature) {
                RebuildRows();
            }
            m_Dirty = false;
        }
        UpdateChrome();
    });

    RebuildRows();
}

RoomLayout::~RoomLayout() {
    *m_Alive = false;
}

ListWidget *RoomLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

void RoomLayout::Refresh() {
    // Force a rebuild on the next frame even if the signature looks unchanged.
    m_Dirty = true;
}

std::size_t RoomLayout::ComputeFloor(std::size_t TimelineSize) const {
    if(m_FloorSet) {
        return m_RenderFloor;
    }
    return TimelineSize > InitialWindow ? TimelineSize - InitialWindow : 0;
}

std::string RoomLayout::ComputeSignature() const {
    const auto Timeline = m_Context->State.GetTimeline(m_RoomId);
    if(Timeline.empty()) {
        return "0";
    }
    const auto Floor = std::min(ComputeFloor(Timeline.size()), Timeline.size() - 1);
    const auto Flags = [](const Matrix::Event &Event) {
        std::string Out;
        Out.push_back(Event.IsDecrypted ? 'D' : '-');
        Out.push_back(Event.IsDecryptionFailed ? 'F' : '-');
        Out.push_back(Event.IsRedacted ? 'R' : '-');
        Out.push_back(Event.IsLocalEcho ? 'L' : '-');
        return Out;
    };
    return std::to_string(Timeline.size()) + '\x1F' + std::to_string(Floor) + '\x1F' + Timeline[Floor].EventId + Flags(Timeline[Floor]) +
           '\x1F' + Timeline.back().EventId + Flags(Timeline.back());
}

void RoomLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    // Read before SetRows: installing rows can fire OnSelectionChanged (row 0
    // is a non-selectable header, so the focus snaps to row 1), which would
    // clear the open-at-bottom intent before the branch below reads it.
    const bool Stick = m_StickToBottom;
    const auto Focus = GetList()->GetFocusIndex();
    const std::string FocusEventId = Focus < m_RowEventIds.size() ? m_RowEventIds[Focus] : "";
    const auto Self = m_Context->State.GetOwnUserId();

    const auto Timeline = m_Context->State.GetTimeline(m_RoomId);
    m_RenderFloor = ComputeFloor(Timeline.size());
    m_FloorSet = true;

    std::unordered_map<std::string, std::size_t> IndexById;
    std::unordered_map<std::string, std::vector<std::size_t>> ReactionIndexes;
    std::unordered_map<std::string, std::size_t> LatestEdit;
    for(std::size_t i = 0; i < Timeline.size(); i++) {
        const auto &Event = Timeline[i];
        IndexById[Event.EventId] = i;
        std::string TargetId;
        std::string Key;
        if(ReactionTarget(Event, TargetId, Key)) {
            ReactionIndexes[TargetId].push_back(i);
        }
        if(Event.Type == "m.room.message") {
            const auto ReplaceTarget = Matrix::GetReplacesEventId(Event);
            if(!ReplaceTarget.empty() && IndexById.count(ReplaceTarget) != 0) {
                const auto Existing = LatestEdit.find(ReplaceTarget);
                if(Existing == LatestEdit.end() || Timeline[Existing->second].Timestamp <= Event.Timestamp) {
                    LatestEdit[ReplaceTarget] = i;
                }
            }
        }
    }

    std::vector<Row::Ref> Rows;
    std::vector<std::string> RowIds;
    std::string LastDay;
    std::string LastMessageSender;
    std::int64_t LastMessageTs = 0;
    bool HavePreviousMessage = false;

    for(std::size_t i = m_RenderFloor; i < Timeline.size(); i++) {
        const auto &Event = Timeline[i];
        if(Event.Type == "m.reaction") {
            continue;
        }
        if(Event.Type == "m.room.message") {
            const auto ReplaceTarget = Matrix::GetReplacesEventId(Event);
            // Edits render on their target row, never standalone.
            if(!ReplaceTarget.empty() && IndexById.count(ReplaceTarget) != 0) {
                continue;
            }
        }

        const std::string Day = Util::FormatDayLabel(Event.Timestamp);
        if(Day != LastDay) {
            LastDay = Day;
            Rows.push_back(std::make_shared<SectionHeaderRow>(Width, Day));
            RowIds.push_back("");
        }

        const bool IsMessage = Event.Type == "m.room.message" || Event.Type == "m.sticker";
        if(!IsMessage) {
            const std::string Line = StateLine(Event);
            if(!Line.empty()) {
                Rows.push_back(std::make_shared<SectionHeaderRow>(Width, Line));
                RowIds.push_back("");
            }
            continue;
        }

        Matrix::Event RenderEvent = Event;
        const auto EditIt = LatestEdit.find(Event.EventId);
        if(EditIt != LatestEdit.end()) {
            const auto &Edit = Timeline[EditIt->second];
            std::string EditedBody;
            if(Edit.Content.contains("m.new_content") && Edit.Content["m.new_content"].contains("body") &&
               Edit.Content["m.new_content"]["body"].is_string()) {
                EditedBody = Edit.Content["m.new_content"]["body"].get<std::string>();
            } else {
                EditedBody = Matrix::GetMessageBody(Edit);
                if(Util::StartsWith(EditedBody, "* ")) {
                    EditedBody = EditedBody.substr(2);
                }
            }
            RenderEvent.Content["body"] = EditedBody;
            RenderEvent.Content.erase("formatted_body");
        }
        if(Event.IsLocalEcho) {
            RenderEvent = WithEchoSuffix(std::move(RenderEvent));
        }

        const bool ShowSender =
            !HavePreviousMessage || Event.Sender != LastMessageSender || (Event.Timestamp - LastMessageTs) > (5 * 60 * 1000);
        const bool OwnMessage = Event.Sender == Self;

        std::vector<Matrix::ReactionGroup> Reactions;
        const auto ReactionIt = ReactionIndexes.find(Event.EventId);
        if(ReactionIt != ReactionIndexes.end()) {
            std::vector<Matrix::Event> Targets;
            Targets.reserve(ReactionIt->second.size());
            for(const auto ReactionIndex : ReactionIt->second) {
                Targets.push_back(Timeline[ReactionIndex]);
            }
            Reactions = Matrix::CollectReactions(Targets, Self);
        }

        // Replies always get a visible quote block, even when the quoted
        // event fell out of the loaded window.
        std::string ReplySender;
        std::string ReplyPreview;
        const auto ReplyTarget = Matrix::GetReplyToEventId(Event);
        if(!ReplyTarget.empty()) {
            const auto TargetIt = IndexById.find(ReplyTarget);
            if(TargetIt != IndexById.end()) {
                const auto &Target = Timeline[TargetIt->second];
                ReplySender = MemberName(Target.Sender);
                ReplyPreview = Matrix::EventPreviewText(Target, Self);
            } else {
                ReplyPreview = "Quoted message not loaded";
            }
        }

        Rows.push_back(std::make_shared<MessageRow>(Width, RenderEvent, MemberName(Event.Sender), ShowSender, OwnMessage, Reactions,
                                                    ReplySender, ReplyPreview, &m_Context->Media));
        RowIds.push_back(Event.EventId);
        LastMessageSender = Event.Sender;
        LastMessageTs = Event.Timestamp;
        HavePreviousMessage = true;
    }

    Rows.push_back(std::make_shared<ComposerRow>(Width, ComposerHeight, ComposerHint));
    RowIds.push_back("");

    m_RowEventIds = std::move(RowIds);
    GetList()->SetRows(std::move(Rows));

    const auto Count = GetList()->GetRowCount();
    if(Count > 0) {
        if(Stick) {
            GetList()->SetFocusIndex(Count - 1);
            GetList()->ScrollToBottom();
        } else {
            // Follow the previously focused event across inserted rows.
            std::size_t NewFocus = Count;
            if(!FocusEventId.empty()) {
                for(std::size_t j = 0; j < m_RowEventIds.size(); j++) {
                    if(m_RowEventIds[j] == FocusEventId) {
                        NewFocus = j;
                        break;
                    }
                }
            }
            if(NewFocus == Count) {
                NewFocus = std::min(Focus, Count - 1);
            }
            const auto Selectable = [&](std::size_t Index) { return !m_RowEventIds[Index].empty() || Index == Count - 1; };
            if(!Selectable(NewFocus)) {
                std::size_t Found = Count;
                for(std::size_t j = NewFocus; j < Count; j++) {
                    if(Selectable(j)) {
                        Found = j;
                        break;
                    }
                }
                for(std::size_t j = NewFocus; j > 0 && Found == Count;) {
                    --j;
                    if(Selectable(j)) {
                        Found = j;
                        break;
                    }
                }
                NewFocus = Found < Count ? Found : Count - 1;
            }
            GetList()->SetFocusIndex(NewFocus);
        }
        if(GetList()->GetFocusIndex() + 3 >= Count) {
            SendReadMarker();
        }
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

std::string RoomLayout::MemberName(const std::string &UserId) const {
    Matrix::RoomMember Member;
    if(m_Context->State.GetMember(m_RoomId, UserId, Member) && !Member.DisplayName.empty()) {
        return Member.DisplayName;
    }
    return UserId;
}

std::string RoomLayout::StateLine(const Matrix::Event &Event) const {
    if(Event.Type == "m.room.member") {
        const std::string Target = Event.StateKey.empty() ? Event.Sender : Event.StateKey;
        std::string Membership;
        if(Event.Content.contains("membership") && Event.Content["membership"].is_string()) {
            Membership = Event.Content["membership"].get<std::string>();
        }
        if(Membership == "join") {
            return MemberName(Target) + " joined";
        }
        if(Membership == "leave") {
            return Target == Event.Sender ? MemberName(Target) + " left"
                                          : MemberName(Target) + " was kicked by " + MemberName(Event.Sender);
        }
        if(Membership == "ban") {
            return MemberName(Target) + " was banned by " + MemberName(Event.Sender);
        }
        if(Membership == "invite") {
            return MemberName(Event.Sender) + " invited " + MemberName(Target);
        }
        return Matrix::EventDisplayText(Event, m_Context->State.GetOwnUserId());
    }
    if(Event.Type == "m.room.name") {
        return "Room name changed";
    }
    if(Event.Type == "m.room.topic") {
        return MemberName(Event.Sender) + " changed the topic";
    }
    if(Event.Type == "m.room.canonical_alias") {
        return MemberName(Event.Sender) + " changed the room address";
    }
    if(Event.Type == "m.room.avatar") {
        return MemberName(Event.Sender) + " changed the avatar";
    }
    if(Event.Type == "m.room.power_levels") {
        return MemberName(Event.Sender) + " changed the power levels";
    }
    if(Event.Type == "m.room.encryption") {
        return MemberName(Event.Sender) + (Event.Content.contains("algorithm") ? " enabled encryption" : " disabled encryption");
    }
    return Matrix::EventDisplayText(Event, m_Context->State.GetOwnUserId());
}

std::string RoomLayout::EffectiveBody(const std::string &EventId) const {
    const auto Timeline = m_Context->State.GetTimeline(m_RoomId);
    const Matrix::Event *Original = nullptr;
    std::string Body;
    std::int64_t EditTs = 0;
    for(const auto &Event : Timeline) {
        if(Event.EventId == EventId && Event.Type == "m.room.message") {
            Original = &Event;
        }
        if(Event.Type != "m.room.message" || Matrix::GetReplacesEventId(Event) != EventId) {
            continue;
        }
        if(!Body.empty() && Event.Timestamp < EditTs) {
            continue;
        }
        if(Event.Content.contains("m.new_content") && Event.Content["m.new_content"].contains("body") &&
           Event.Content["m.new_content"]["body"].is_string()) {
            Body = Event.Content["m.new_content"]["body"].get<std::string>();
        } else {
            Body = Matrix::GetMessageBody(Event);
            if(Util::StartsWith(Body, "* ")) {
                Body = Body.substr(2);
            }
        }
        EditTs = Event.Timestamp;
    }
    if(!Body.empty()) {
        return Body;
    }
    return Original != nullptr ? Matrix::GetMessageBody(*Original) : std::string{};
}

void RoomLayout::UpdateChrome() {
    Matrix::RoomSummary Summary;
    if(m_Context->State.GetRoomSummary(m_RoomId, Summary)) {
        const auto Name = Matrix::RoomDisplayName(Summary);
        if(Name != m_LastHeader) {
            m_LastHeader = Name;
            m_Header->SetText(Name);
        }
        const bool Encrypted = m_Context->State.IsRoomEncrypted(m_RoomId);
        std::string Meta = std::to_string(Summary.MemberCount) + " members";
        if(Encrypted) {
            Meta += " | E2EE";
        }
        if(Meta != m_LastMeta) {
            m_LastMeta = Meta;
            m_Meta->SetColor(Encrypted ? Theme::Accent : Theme::TextSecondary);
            m_Meta->SetText(Meta);
            m_Meta->SetVisible(true);
            m_Meta->SetX(Theme::ScreenWidth - Theme::ScreenMargin - m_Meta->GetWidth());
        }
    }

    auto Typers = m_Context->State.GetTypingUsers(m_RoomId);
    const auto Self = m_Context->State.GetOwnUserId();
    Typers.erase(std::remove(Typers.begin(), Typers.end(), Self), Typers.end());
    std::string Typing;
    if(Typers.size() == 1) {
        Typing = MemberName(Typers[0]) + " is typing...";
    } else if(Typers.size() == 2) {
        Typing = MemberName(Typers[0]) + " and " + MemberName(Typers[1]) + " are typing...";
    } else if(Typers.size() > 2) {
        Typing = MemberName(Typers[0]) + " and " + std::to_string(Typers.size() - 1) + " others are typing...";
    }
    if(Typing != m_LastTyping) {
        m_LastTyping = Typing;
        m_Typing->SetVisible(!Typing.empty());
        if(!Typing.empty()) {
            m_Typing->SetText(Typing);
        }
    }

    // The reply/edit context only changes on user actions, so the timeline
    // snapshot for the preview is taken exactly then.
    const std::string ComposerKey = m_ReplyToEventId + '\x1F' + m_EditEventId;
    if(ComposerKey != m_LastComposerKey) {
        m_LastComposerKey = ComposerKey;
        m_ComposerPreview.clear();
        if(!m_EditEventId.empty()) {
            m_ComposerPreview = "Editing: " + Util::TruncateWithEllipsis(EffectiveBody(m_EditEventId), 48);
        } else if(!m_ReplyToEventId.empty()) {
            m_ComposerPreview = "Reply: " + Util::TruncateWithEllipsis(m_ReplySenderName, 24);
        }
    }
    const std::string ComposerText = m_ComposerPreview.empty() ? "Message" : m_ComposerPreview;
    if(ComposerText != m_LastComposerText) {
        m_LastComposerText = ComposerText;
        m_Composer->SetColor(m_ComposerPreview.empty() ? Theme::TextFaint : Theme::TextPrimary);
        m_Composer->SetText(ComposerText);
    }
    m_Composer->SetVisible(GetList()->IsAtBottom());

    UpdateStatusLine();
}

void RoomLayout::UpdateStatusLine() {
    std::string Text;
    pu::ui::Color Color = Theme::TextSecondary;
    if(!m_EditEventId.empty()) {
        Text = "Editing message";
        Color = Theme::Warning;
    } else if(!m_ReplyToEventId.empty()) {
        Text = "Replying to " + (m_ReplySenderName.empty() ? std::string("message") : m_ReplySenderName);
        Color = Theme::Accent;
    } else if(!m_TransientStatus.empty()) {
        Text = m_TransientStatus;
        Color = Theme::Danger;
    } else if(m_LoadingOlder) {
        Text = "Loading older messages...";
    }
    if(Text == m_LastStatus) {
        return;
    }
    m_LastStatus = Text;
    m_StatusLine->SetVisible(!Text.empty());
    if(!Text.empty()) {
        m_StatusLine->SetColor(Color);
        m_StatusLine->SetText(Text);
    }
}

void RoomLayout::OnComposerActivated() {
    auto Ctx = m_Context;
    const std::string Room = m_RoomId;
    // Fire-and-forget: the server drops the typing flag on its own timeout.
    Core::JobQueue::Instance().Enqueue([Ctx, Room]() { Ctx->Api.SetTyping(Room, true, 30); });

    Matrix::RoomSummary Summary;
    std::string Guide = Room;
    if(m_Context->State.GetRoomSummary(Room, Summary)) {
        Guide = Matrix::RoomDisplayName(Summary);
    }

    // "Current" text: a pending edit body survives into the composer.
    std::string Text;
    if(!m_EditEventId.empty()) {
        Text = EffectiveBody(m_EditEventId);
    }
    if(!Keyboard::QueryLongText("Message", Guide, Text)) {
        if(!m_ReplyToEventId.empty() || !m_EditEventId.empty()) {
            m_ReplyToEventId.clear();
            m_ReplySenderName.clear();
            m_EditEventId.clear();
            UpdateStatusLine();
        }
        return;
    }
    if(Util::Trim(Text).empty()) {
        if(!m_ReplyToEventId.empty() || !m_EditEventId.empty()) {
            m_ReplyToEventId.clear();
            m_ReplySenderName.clear();
            m_EditEventId.clear();
            UpdateStatusLine();
        }
        return;
    }
    SendMessage(Text, m_ReplyToEventId, m_EditEventId);
    m_ReplyToEventId.clear();
    m_ReplySenderName.clear();
    m_EditEventId.clear();
    UpdateStatusLine();
}

void RoomLayout::SendMessage(const std::string &Body, const std::string &ReplyToEventId, const std::string &EditEventId) {
    nlohmann::json Content;
    const bool IsEdit = !EditEventId.empty();
    if(IsEdit) {
        Content["msgtype"] = "m.text";
        Content["body"] = "* " + Body;
        Content["m.new_content"]["msgtype"] = "m.text";
        Content["m.new_content"]["body"] = Body;
        Content["m.relates_to"]["rel_type"] = "m.replace";
        Content["m.relates_to"]["event_id"] = EditEventId;
    } else {
        Content["msgtype"] = "m.text";
        Content["body"] = Body;
        if(!ReplyToEventId.empty()) {
            // Spec rich replies: m.relates_to.m.in_reply_to.event_id (no
            // rel_type). The replied-to sender also goes into m.mentions so
            // they get notified; own ID is excluded, outgoing messages cannot
            // self-notify.
            Content["m.relates_to"]["m.in_reply_to"]["event_id"] = ReplyToEventId;
            const auto Own = m_Context->State.GetOwnUserId();
            for(const auto &Target : m_Context->State.GetTimeline(m_RoomId)) {
                if(Target.EventId == ReplyToEventId) {
                    if(Target.Sender != Own) {
                        Content["m.mentions"]["user_ids"] = nlohmann::json::array({Target.Sender});
                    }
                    break;
                }
            }
        }
    }

    const std::string Txn = RandomTxn();
    Matrix::Event Echo;
    Echo.EventId = "local:" + Txn;
    Echo.RoomId = m_RoomId;
    Echo.Sender = m_Context->State.GetOwnUserId();
    Echo.Type = "m.room.message";
    Echo.Timestamp = Util::NowMs();
    Echo.Content = Content;
    Echo.IsLocalEcho = true;
    Echo.TransactionId = Txn;
    m_Context->State.UpsertLocalEcho(Echo);

    auto Ctx = m_Context;
    const std::string Room = m_RoomId;
    const bool Encrypted = m_Context->State.IsRoomEncrypted(Room);
    Core::JobQueue::Instance().Enqueue([Ctx, Room, Content, Txn, Encrypted]() {
        bool Ok = false;
        std::string EventId;
        std::string FailReason; // set when encryption itself refused the event
        if(Encrypted) {
            std::vector<std::string> MemberIds;
            for(const auto &Member : Ctx->State.GetMembers(Room)) {
                if(Member.Membership == Matrix::MembershipState::Join || Member.Membership == Matrix::MembershipState::Invite) {
                    MemberIds.push_back(Member.UserId);
                }
            }
            // The megolm payload carries the ORIGINAL event type: receivers set
            // their event type from the decrypted payload, so "m.room.message"
            // must go in here (the outer send stays "m.room.encrypted").
            nlohmann::json EncryptedContent;
            if(Ctx->E2ee.EncryptRoomEvent(Room, MemberIds, "m.room.message", Content, EncryptedContent, &FailReason)) {
                auto Sent = Ctx->Api.SendEvent(Room, "m.room.encrypted", EncryptedContent, Txn);
                Ok = Sent.Ok;
                if(Ok) {
                    EventId = Sent.Value;
                }
            }
        } else {
            auto Sent = Ctx->Api.SendEvent(Room, "m.room.message", Content, Txn);
            Ok = Sent.Ok;
            if(Ok) {
                EventId = Sent.Value;
            }
        }
        Core::Dispatcher::Instance().Post([Ctx, Room, Txn, Ok, EventId, FailReason]() {
            if(Ok) {
                Ctx->State.ResolveLocalEcho(Room, Txn, EventId);
            } else {
                Ctx->State.RemoveLocalEcho(Room, Txn);
                ShowToast(Ctx->Nav.GetApp(), FailReason.empty() ? "Message not sent" : FailReason);
            }
        });
    });
}

void RoomLayout::OnMessageActivated(std::size_t Index) {
    if(Index >= m_RowEventIds.size()) {
        return;
    }
    const std::string EventId = m_RowEventIds[Index];
    if(EventId.empty()) {
        return;
    }

    const auto Timeline = m_Context->State.GetTimeline(m_RoomId);
    const Matrix::Event *Target = nullptr;
    for(const auto &Event : Timeline) {
        if(Event.EventId == EventId) {
            Target = &Event;
            break;
        }
    }
    if(Target == nullptr) {
        return;
    }

    auto &App = m_Context->Nav.GetApp();
    const auto Self = m_Context->State.GetOwnUserId();

    std::vector<std::string> Options;
    if(!m_ReplyToEventId.empty()) {
        Options.push_back("Cancel reply");
    }
    if(!m_EditEventId.empty()) {
        Options.push_back("Cancel edit");
    }
    Options.push_back("Reply");
    Options.push_back("React");
    const bool Own = Target->Sender == Self && !Target->IsLocalEcho;
    if(Own && Target->Type == "m.room.message") {
        Options.push_back("Edit");
    }
    const auto Levels = m_Context->State.GetPowerLevels(m_RoomId);
    if(!Target->IsLocalEcho && (Own || Levels.GetUserLevel(Self) >= Levels.Redact)) {
        Options.push_back("Redact");
    }

    std::string AttachmentUrl;
    if(Target->Content.contains("msgtype") && Target->Content["msgtype"].is_string() && Target->Content.contains("url") &&
       Target->Content["url"].is_string()) {
        const auto MsgType = Target->Content["msgtype"].get<std::string>();
        const auto Url = Target->Content["url"].get<std::string>();
        if((MsgType == "m.image" || MsgType == "m.file" || MsgType == "m.audio" || MsgType == "m.video") &&
           Util::StartsWith(Url, "mxc://")) {
            AttachmentUrl = Url;
            Options.push_back("Save attachment");
        }
    }

    const std::string Choice = ShowOptions(App, "Message", "", Options);
    if(Choice.empty()) {
        return;
    }

    if(Choice == "Cancel reply") {
        m_ReplyToEventId.clear();
        m_ReplySenderName.clear();
        UpdateStatusLine();
        return;
    }
    if(Choice == "Cancel edit") {
        m_EditEventId.clear();
        UpdateStatusLine();
        return;
    }
    if(Choice == "Reply") {
        m_ReplyToEventId = EventId;
        m_ReplySenderName = MemberName(Target->Sender);
        m_EditEventId.clear();
        UpdateStatusLine();
        return;
    }

    if(Choice == "React") {
        std::vector<std::string> MyReactionIds;
        for(const auto &Event : Timeline) {
            std::string TargetId;
            std::string Key;
            if(Event.Sender == Self && ReactionTarget(Event, TargetId, Key) && TargetId == EventId) {
                MyReactionIds.push_back(Event.EventId);
            }
        }
        std::vector<std::string> Keys{"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82",
                                      "\xF0\x9F\x8E\x89", "\xF0\x9F\x91\x80",         "\xE2\x9C\x85"};
        if(!MyReactionIds.empty()) {
            Keys.push_back("Remove my reaction");
        }
        auto Ctx = m_Context;
        const std::string Room = m_RoomId;
        // The generic options dialog draws through the standard font (tofu for
        // emoji): the dedicated picker renders the choices with the color font.
        m_Context->Nav.Push(ReactionPickerLayout::New(Ctx, Keys, [Ctx, Room, EventId, MyReactionIds](const std::string &Pick) {
            if(Pick.empty()) {
                return;
            }
            if(Pick == "Remove my reaction") {
                Core::JobQueue::Instance().Enqueue([Ctx, Room, MyReactionIds]() {
                    bool Failed = false;
                    for(const auto &ReactionId : MyReactionIds) {
                        if(!Ctx->Api.RedactEvent(Room, ReactionId, "", RandomTxn()).Ok) {
                            Failed = true;
                        }
                    }
                    if(Failed) {
                        Core::Dispatcher::Instance().Post([Ctx]() { ShowToast(Ctx->Nav.GetApp(), "Could not remove reaction"); });
                    }
                });
                return;
            }
            nlohmann::json Content;
            Content["m.relates_to"]["rel_type"] = "m.annotation";
            Content["m.relates_to"]["event_id"] = EventId;
            Content["m.relates_to"]["key"] = Pick;
            const bool Enc = Ctx->State.IsRoomEncrypted(Room);
            Core::JobQueue::Instance().Enqueue([Ctx, Room, Content, Enc]() {
                std::string FailReason;
                bool Sent = false;
                if(Enc) {
                    std::vector<std::string> MemberIds;
                    for(const auto &Member : Ctx->State.GetMembers(Room)) {
                        if(Member.Membership == Matrix::MembershipState::Join || Member.Membership == Matrix::MembershipState::Invite) {
                            MemberIds.push_back(Member.UserId);
                        }
                    }
                    nlohmann::json EncryptedContent;
                    if(Ctx->E2ee.EncryptRoomEvent(Room, MemberIds, "m.reaction", Content, EncryptedContent, &FailReason)) {
                        Sent = Ctx->Api.SendEvent(Room, "m.room.encrypted", EncryptedContent, RandomTxn()).Ok;
                    }
                } else {
                    Sent = Ctx->Api.SendEvent(Room, "m.reaction", Content, RandomTxn()).Ok;
                }
                if(!Sent) {
                    Core::Dispatcher::Instance().Post(
                        [Ctx, FailReason]() { ShowToast(Ctx->Nav.GetApp(), FailReason.empty() ? "Reaction not sent" : FailReason); });
                }
            });
        }));
        return;
    }

    if(Choice == "Edit") {
        Matrix::RoomSummary Summary;
        std::string Guide = m_RoomId;
        if(m_Context->State.GetRoomSummary(m_RoomId, Summary)) {
            Guide = Matrix::RoomDisplayName(Summary);
        }
        std::string Body = EffectiveBody(EventId);
        if(!Keyboard::QueryLongText("Edit message", Guide, Body)) {
            return;
        }
        if(Util::Trim(Body).empty()) {
            m_EditEventId.clear();
            UpdateStatusLine();
            return;
        }
        SendMessage(Body, "", EventId);
        m_ReplyToEventId.clear();
        m_ReplySenderName.clear();
        m_EditEventId.clear();
        UpdateStatusLine();
        return;
    }

    if(Choice == "Redact") {
        if(!Confirm(App, "Redact message", "Delete this message for everyone?", "Redact", "Cancel")) {
            return;
        }
        auto Ctx = m_Context;
        const std::string Room = m_RoomId;
        Core::JobQueue::Instance().Enqueue([Ctx, Room, EventId]() {
            const bool Done = Ctx->Api.RedactEvent(Room, EventId, "", RandomTxn()).Ok;
            Core::Dispatcher::Instance().Post([Ctx, Done]() {
                if(Done) {
                    ShowToast(Ctx->Nav.GetApp(), "Message redacted");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Redact failed", "The server rejected the redaction");
                }
            });
        });
        return;
    }

    if(Choice == "Save attachment") {
        std::string Candidate;
        if(Target->Content.contains("body") && Target->Content["body"].is_string()) {
            Candidate = Target->Content["body"].get<std::string>();
        }
        if(Candidate.find('.') == std::string::npos) {
            const auto Slash = AttachmentUrl.find_last_of('/');
            Candidate = Slash == std::string::npos ? std::string{} : AttachmentUrl.substr(Slash + 1);
        }
        const std::string Name = SafeFileName(Candidate);
        auto Ctx = m_Context;
        Ctx->Media.LoadMxc(AttachmentUrl, 0, 0, [Ctx, Name](bool Success, const ByteArray &Data) {
            if(!Success) {
                ShowToast(Ctx->Nav.GetApp(), "Download failed");
                return;
            }
            Core::JobQueue::Instance().Enqueue([Ctx, Name, Data]() {
                const bool Saved = SaveDownload(Name, Data);
                Core::Dispatcher::Instance().Post([Ctx, Name, Saved]() {
                    const std::string Dir = Store::SecureStore::GetRootPath() + "/Downloads";
                    ShowToast(Ctx->Nav.GetApp(), Saved ? ("Saved to " + Dir + "/" + Name) : "Could not save the file");
                });
            });
        });
        return;
    }
}

void RoomLayout::LoadOlder() {
    if(m_LoadingOlder || m_PaginationDone) {
        return;
    }
    const std::string From = m_PrevBatch.empty() ? m_Context->Sync.GetSinceToken() : m_PrevBatch;
    if(From.empty()) {
        m_PaginationDone = true;
        return;
    }
    m_LoadingOlder = true;
    UpdateStatusLine();

    const auto OldSize = m_Context->State.GetTimeline(m_RoomId).size();
    const auto OldFocus = GetList()->GetFocusIndex();
    const std::string FocusEventId = OldFocus < m_RowEventIds.size() ? m_RowEventIds[OldFocus] : "";

    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string Room = m_RoomId;
    Core::JobQueue::Instance().Enqueue([this, Ctx, Alive, Room, From, OldSize, OldFocus, FocusEventId]() {
        auto Page = Ctx->Api.GetMessages(Room, From, "", true, 50);
        Core::Dispatcher::Instance().Post([this, Ctx, Alive, Room, From, Page, OldSize, OldFocus, FocusEventId]() {
            if(!*Alive) {
                return;
            }
            m_LoadingOlder = false;
            if(!Page.Ok) {
                m_TransientStatus = "Could not load older messages";
                UpdateStatusLine();
                ShowToast(Ctx->Nav.GetApp(), "Could not load older messages");
                return;
            }
            // dir=b returns newest -> oldest; reverse to keep the timeline chronological.
            auto Older = Page.Value.Events;
            std::reverse(Older.begin(), Older.end());
            if(Page.Value.PrevBatch.empty() || Page.Value.PrevBatch == From) {
                m_PaginationDone = true;
            }
            m_PrevBatch = Page.Value.PrevBatch;
            Ctx->State.ApplyHistory(Room, Older, Page.Value.PrevBatch);

            const auto NewSize = Ctx->State.GetTimeline(Room).size();
            const std::size_t Inserted = NewSize > OldSize ? (NewSize - OldSize) : 0;
            if(Inserted > 0) {
                // The freshly loaded page extends below the initial window.
                m_RenderFloor = 0;
                m_FloorSet = true;
            }
            m_TransientStatus.clear();
            RebuildRows();

            const auto Count = GetList()->GetRowCount();
            if(Count > 0) {
                std::size_t NewFocus = std::min(OldFocus + Inserted, Count - 1);
                if(!FocusEventId.empty()) {
                    for(std::size_t i = 0; i < m_RowEventIds.size(); i++) {
                        if(m_RowEventIds[i] == FocusEventId) {
                            NewFocus = i;
                            break;
                        }
                    }
                }
                GetList()->SetFocusIndex(NewFocus);
            }
            UpdateStatusLine();
        });
    });
}

void RoomLayout::SendReadMarker() {
    const auto Count = GetList()->GetRowCount();
    if(Count == 0 || m_RowEventIds.size() != Count) {
        return;
    }
    const auto Focus = GetList()->GetFocusIndex();
    auto Usable = [](const std::string &Id) { return !Id.empty() && !Util::StartsWith(Id, "local:"); };

    std::string EventId;
    for(std::size_t i = Focus; i < Count && EventId.empty(); i++) {
        if(Usable(m_RowEventIds[i])) {
            EventId = m_RowEventIds[i];
        }
    }
    for(std::size_t i = Focus; i > 0 && EventId.empty();) {
        --i;
        if(Usable(m_RowEventIds[i])) {
            EventId = m_RowEventIds[i];
        }
    }
    if(EventId.empty() || EventId == m_LastMarkedEventId) {
        return;
    }
    m_LastMarkedEventId = EventId;

    auto Ctx = m_Context;
    const std::string Room = m_RoomId;
    Core::JobQueue::Instance().Enqueue([Ctx, Room, EventId]() {
        Ctx->Api.SendReadReceipt(Room, EventId);
        Ctx->Api.SetReadMarkers(Room, EventId, EventId);
    });
}

} // namespace NxMatrix::Ui