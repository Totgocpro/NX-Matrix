// Room details: topic/name editing gated by power levels, member list entry,
// favourite/low-priority tags, encryption info and leave+forget.
#include "NxMatrix/Ui/Layouts/RoomDetailsLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/MemberListLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <map>

namespace NxMatrix::Ui {

RoomDetailsLayout::RoomDetailsLayout(std::shared_ptr<AppContext> Context, const std::string &RoomId)
    : m_Context(std::move(Context)), m_RoomId(RoomId) {
    this->SetBackgroundColor(Theme::Background);

    auto Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "Room details");
    Header->SetFont(Theme::FontHeading);
    Header->SetColor(Theme::Accent);
    this->Add(Header);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnRowActivated(Index); });

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A: select");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    // Session changes arrive as version bumps; rebuild only when what we
    // display actually changed.
    this->AddRenderCallback([this]() {
        const auto Version = m_Context->State.GetVersion();
        if(Version == m_LastVersion) {
            return;
        }
        m_LastVersion = Version;
        if(ComputeSignature() != m_LastSignature) {
            RebuildRows();
        }
    });

    RebuildRows();
}

RoomDetailsLayout::~RoomDetailsLayout() {
    *m_Alive = false;
}

void RoomDetailsLayout::Refresh() {
    RebuildRows();
}

ListWidget *RoomDetailsLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

std::string RoomDetailsLayout::ComputeSignature() const {
    Matrix::RoomSummary Summary;
    if(!m_Context->State.GetRoomSummary(m_RoomId, Summary)) {
        return {};
    }
    return Summary.Name + '\x1F' + Summary.Topic + '\x1F' + std::to_string(Summary.MemberCount) + '\x1F' + //
           (Summary.IsFavourite ? "F" : "-") + (Summary.IsLowPriority ? "L" : "-") + (Summary.IsEncrypted ? "E" : "-") +
           '\x1F' + std::to_string(EligibleSpaces(Summary).size());
}

std::vector<std::string> RoomDetailsLayout::EligibleSpaces(const Matrix::RoomSummary &Summary) const {
    std::vector<std::string> Out;
    if(Summary.Membership != Matrix::MembershipState::Join) {
        return Out; // only joined rooms can be linked into a space
    }
    for(const auto &Space : m_Context->State.GetRoomSummaries()) {
        if(!Space.IsSpace || Space.Membership != Matrix::MembershipState::Join || Space.RoomId == m_RoomId) {
            continue;
        }
        if(!m_Context->State.CanSendState(Space.RoomId, "m.space.child")) {
            continue; // insufficient power level in that space
        }
        bool HasChild = false;
        for(const auto &Child : m_Context->State.GetSpaceChildren(Space.RoomId)) {
            if(Child.RoomId == m_RoomId) {
                HasChild = true;
                break;
            }
        }
        if(!HasChild) {
            Out.push_back(Space.RoomId);
        }
    }
    return Out;
}

void RoomDetailsLayout::RebuildRows() {
    Matrix::RoomSummary Summary;
    m_Context->State.GetRoomSummary(m_RoomId, Summary);
    if(Summary.MemberCount <= 0) {
        Summary.MemberCount = static_cast<std::int64_t>(m_Context->State.GetMembers(m_RoomId).size());
    }

    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();

    std::vector<Row::Ref> Rows;
    m_RowTags.clear();

    auto Push = [&](RowKind Kind, const Row::Ref &RowElem) {
        Rows.push_back(RowElem);
        m_RowTags.push_back(Kind);
    };

    Push(RowKind::Header, std::make_shared<SectionHeaderRow>(Width, "Room"));
    Push(RowKind::Topic, std::make_shared<SimpleRow>(Width, "Topic", Util::TruncateWithEllipsis(Summary.Topic, 48)));
    Push(RowKind::Name, std::make_shared<SimpleRow>(Width, "Name", Summary.Name));
    Push(RowKind::Members, std::make_shared<SimpleRow>(Width, "Members", std::to_string(Summary.MemberCount)));
    Push(RowKind::Invite, std::make_shared<SimpleRow>(Width, "Invite user"));

    Push(RowKind::Header, std::make_shared<SectionHeaderRow>(Width, "Flags"));
    Push(RowKind::Favourite, std::make_shared<ToggleRow>(Width, "Favourite", Summary.IsFavourite));
    Push(RowKind::LowPriority, std::make_shared<ToggleRow>(Width, "Low priority", Summary.IsLowPriority));

    Push(RowKind::Header, std::make_shared<SectionHeaderRow>(Width, "Security"));
    Push(RowKind::Encryption,
         std::make_shared<SimpleRow>(Width, "Encryption", Summary.IsEncrypted ? "End-to-end encrypted" : "Not encrypted",
                                     Summary.IsEncrypted ? "Messages are secured with Megolm (device not verified)"
                                                         : "Anyone in this room can read the messages"));

    // Space membership (m.space.child): only offered while a joined space
    // can still receive this room.
    const auto Spaces = EligibleSpaces(Summary);
    if(!Spaces.empty()) {
        Push(RowKind::Header, std::make_shared<SectionHeaderRow>(Width, "Spaces"));
        Push(RowKind::AddToSpace, std::make_shared<SimpleRow>(Width, "Add to space",
                                                               Spaces.size() == 1 ? "Add" : std::to_string(Spaces.size()) + " spaces",
                                                               "Link this room into a space"));
    }

    Push(RowKind::Header, std::make_shared<SectionHeaderRow>(Width, "Danger zone"));
    Push(RowKind::Leave, std::make_shared<SimpleRow>(Width, "Leave room", "Leave"));

    GetList()->SetRows(std::move(Rows));
    if(Focus < m_RowTags.size() && m_RowTags[Focus] != RowKind::Header) {
        GetList()->SetFocusIndex(Focus);
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

void RoomDetailsLayout::OnRowActivated(std::size_t Index) {
    if(Index >= m_RowTags.size()) {
        return;
    }
    auto &App = m_Context->Nav.GetApp();
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string Room = m_RoomId;

    switch(m_RowTags[Index]) {
    case RowKind::Topic: {
        if(!m_Context->State.CanSendState(m_RoomId, "m.room.topic")) {
            ShowError(App, "Not allowed", "You cannot change the topic of this room.");
            return;
        }
        Matrix::RoomSummary Summary;
        m_Context->State.GetRoomSummary(m_RoomId, Summary);
        std::string Topic = Summary.Topic;
        if(!Keyboard::QueryLongText("Topic", "Room topic", Topic)) {
            return;
        }
        if(Topic == Summary.Topic) {
            return;
        }
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, Topic]() {
            const auto Status = Ctx->Api.SendStateEvent(Room, "m.room.topic", "", nlohmann::json{{"topic", Topic}});
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status]() {
                if(!*Alive) {
                    return;
                }
                if(Status.Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Topic updated");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Topic not saved", Status.Error.ToDisplayString());
                }
            });
        });
        break;
    }
    case RowKind::Name: {
        if(!m_Context->State.CanSendState(m_RoomId, "m.room.name")) {
            ShowError(App, "Not allowed", "You cannot change the name of this room.");
            return;
        }
        Matrix::RoomSummary Summary;
        m_Context->State.GetRoomSummary(m_RoomId, Summary);
        std::string Name = Summary.Name;
        if(!Keyboard::QueryText("Room name", "Name of the room", Name)) {
            return;
        }
        if(Name == Summary.Name) {
            return;
        }
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, Name]() {
            const auto Status = Ctx->Api.SendStateEvent(Room, "m.room.name", "", nlohmann::json{{"name", Name}});
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status]() {
                if(!*Alive) {
                    return;
                }
                if(Status.Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Room name updated");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Name not saved", Status.Error.ToDisplayString());
                }
            });
        });
        break;
    }
    case RowKind::Members: {
        m_Context->Nav.Push(MemberListLayout::New(m_Context, m_RoomId));
        break;
    }
    case RowKind::Invite: {
        std::string UserId;
        if(!Keyboard::QueryText("Invite", "@user:server.org", UserId)) {
            return;
        }
        UserId = Util::Trim(UserId);
        if(UserId.empty()) {
            return;
        }
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, UserId]() {
            const auto Status = Ctx->Api.InviteUser(Room, UserId);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status]() {
                if(!*Alive) {
                    return;
                }
                if(Status.Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Invitation sent");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Invite failed", Status.Error.ToDisplayString());
                }
            });
        });
        break;
    }
    case RowKind::Favourite:
    case RowKind::LowPriority: {
        Matrix::RoomSummary Summary;
        if(!m_Context->State.GetRoomSummary(m_RoomId, Summary)) {
            return;
        }
        const bool Favourite = (m_RowTags[Index] == RowKind::Favourite);
        const bool Enable = Favourite ? !Summary.IsFavourite : !Summary.IsLowPriority;
        const std::string Tag = Favourite ? "m.favourite" : "m.lowpriority";
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, Tag, Enable]() {
            const auto Status = Enable ? Ctx->Api.SetTag(Room, Tag, 1.0) : Ctx->Api.RemoveTag(Room, Tag);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status]() {
                if(!*Alive) {
                    return;
                }
                if(!Status.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Tag not saved", Status.Error.ToDisplayString());
                }
            });
        });
        break;
    }
    case RowKind::Encryption: {
        Matrix::RoomSummary Summary;
        m_Context->State.GetRoomSummary(m_RoomId, Summary);
        ShowMessage(App, "Encryption",
                    Summary.IsEncrypted ? "Messages are secured with Megolm (device not verified)"
                                        : "Anyone in this room can read the messages");
        break;
    }
    case RowKind::AddToSpace: {
        Matrix::RoomSummary Summary;
        if(!m_Context->State.GetRoomSummary(m_RoomId, Summary)) {
            return;
        }
        const auto Spaces = EligibleSpaces(Summary);
        if(Spaces.empty()) {
            ShowMessage(App, "No spaces", "No joined space can receive this room yet.");
            return;
        }
        // Dialog options must be unique labels: name on its own can collide,
        // so duplicates get a short room-id suffix (ids are unique).
        std::map<std::string, std::string> LabelToSpace;
        std::vector<std::string> Labels;
        for(const auto &SpaceId : Spaces) {
            Matrix::RoomSummary Space;
            m_Context->State.GetRoomSummary(SpaceId, Space);
            std::string Name = Matrix::RoomDisplayName(Space);
            bool Duplicate = false;
            for(const auto &Other : Spaces) {
                if(Other == SpaceId) {
                    continue;
                }
                Matrix::RoomSummary OtherSummary;
                if(m_Context->State.GetRoomSummary(Other, OtherSummary) && Matrix::RoomDisplayName(OtherSummary) == Name) {
                    Duplicate = true;
                    break;
                }
            }
            if(Duplicate) {
                Name += " (" + Util::TruncateWithEllipsis(SpaceId, 16) + ")";
            }
            LabelToSpace[Name] = SpaceId;
            Labels.push_back(Name);
        }
        const std::string Choice = ShowOptions(App, "Add to space", "Pick the space for \"" + Matrix::RoomDisplayName(Summary) + "\"",
                                               Labels);
        if(Choice.empty() || LabelToSpace.count(Choice) == 0) {
            return;
        }
        const std::string SpaceId = LabelToSpace[Choice];
        // via: the local server, so others can route to this child later.
        const std::string OwnId = m_Context->State.GetOwnUserId();
        const auto Colon = OwnId.find(':');
        const std::string OwnServer = Colon == std::string::npos ? OwnId : OwnId.substr(Colon + 1);
        const nlohmann::json ChildContent = nlohmann::json{{"via", nlohmann::json::array({OwnServer})}};
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, SpaceId, Room, ChildContent]() {
            const auto Status = Ctx->Api.SendStateEvent(SpaceId, "m.space.child", Room, ChildContent);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status, SpaceId, Room, ChildContent]() {
                if(!*Alive) {
                    return;
                }
                if(!Status.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Not added", Status.Error.ToDisplayString());
                    return;
                }
                // Apply now; the sync echo of the same event is idempotent.
                Ctx->State.ApplyFetchedState(SpaceId, "m.space.child", ChildContent, Room, Util::NowMs());
                ShowToast(Ctx->Nav.GetApp(), "Added to the space");
            });
        });
        break;
    }
    case RowKind::Leave: {
        Matrix::RoomSummary Summary;
        m_Context->State.GetRoomSummary(m_RoomId, Summary);
        const auto RoomName = Summary.Name.empty() ? m_RoomId : Summary.Name;
        if(!Confirm(App, "Leave room", "Leave \"" + RoomName + "\"? You will stop receiving its messages.", "Leave")) {
            return;
        }
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room]() {
            const auto Leave = Ctx->Api.LeaveRoom(Room);
            // Forgetting is best-effort: the room may already be gone.
            (void)Ctx->Api.ForgetRoom(Room);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Leave]() {
                if(!*Alive) {
                    return;
                }
                if(!Leave.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Leave failed", Leave.Error.ToDisplayString());
                    return;
                }
                // Details -> room -> back to the room list.
                Ctx->Nav.Pop();
                Ctx->Nav.Pop();
            });
        });
        break;
    }
    case RowKind::Header:
    default:
        break;
    }
}

} // namespace NxMatrix::Ui