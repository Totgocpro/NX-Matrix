// Space view: ordered m.space.child rooms of a space-room (spec v1.19
// §Spaces). Local state drives the rows; one /hierarchy fetch decorates the
// children the session does not know (joined elsewhere, never joined).
#include "NxMatrix/Ui/Layouts/SpaceLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Layouts/RoomDetailsLayout.hpp"
#include "NxMatrix/Ui/Layouts/RoomLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <algorithm>

namespace NxMatrix::Ui {

namespace {

// Join rules the user may attempt to join directly (spec §Join rules).
// "invite"/"private" only accept invitations; restricted servers decide.
bool JoinRuleAllows(const std::string &Rule) {
    return Rule.empty() || Rule == "public" || Rule == "restricted" || Rule == "knock" || Rule == "knock_restricted";
}

} // namespace

SpaceLayout::SpaceLayout(std::shared_ptr<AppContext> Context, const std::string &SpaceId, std::vector<std::string> Ancestors)
    : m_Context(std::move(Context)), m_SpaceId(SpaceId), m_Ancestors(std::move(Ancestors)) {
    this->SetBackgroundColor(Theme::Background);

    m_Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 24, m_SpaceId);
    m_Header->SetFont(Theme::FontHeading);
    m_Header->SetColor(Theme::Accent);
    this->Add(m_Header);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnRowActivated(Index); });

    m_EmptyText = pu::ui::elm::TextBlock::New(0, Theme::ListTop + 160, "No rooms in this space yet");
    m_EmptyText->SetFont(Theme::FontBody);
    m_EmptyText->SetColor(Theme::TextSecondary);
    const auto EmptyWidth = pu::ui::render::GetTextWidth(Theme::FontBody, m_EmptyText->GetText());
    m_EmptyText->SetX(std::max(0, (Theme::ScreenWidth - EmptyWidth) / 2));
    m_EmptyText->SetVisible(false);
    this->Add(m_EmptyText);

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A Open  B Back  X Space actions");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        if(KeysDown & HidNpadButton_X) {
            ShowSpaceActions();
        }
    });

    // Rebuild when the session moved and the rendered rows actually changed
    // (child add/remove lands with the next sync batch or optimistically).
    this->AddRenderCallback([this]() {
        const auto Version = m_Context->State.GetVersion();
        if(m_Dirty || Version != m_LastVersion) {
            m_LastVersion = Version;
            if(m_Dirty || ComputeSignature() != m_LastSignature) {
                RebuildRows();
            }
            m_Dirty = false;
        }
    });

    RebuildRows();
    FetchHierarchy();
}

SpaceLayout::~SpaceLayout() {
    *m_Alive = false;
}

ListWidget *SpaceLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

bool SpaceLayout::WouldLoop(const std::string &ChildId) const {
    // The path so far plus this screen: reopening an ancestor would recurse
    // forever (spec: loops are prohibited but must be broken client-side).
    return ChildId == m_SpaceId || std::find(m_Ancestors.begin(), m_Ancestors.end(), ChildId) != m_Ancestors.end();
}

std::vector<std::string> SpaceLayout::ChildVia(const std::string &ChildId) const {
    for(const auto &Child : m_Context->State.GetSpaceChildren(m_SpaceId)) {
        if(Child.RoomId == ChildId) {
            return Child.Via;
        }
    }
    return {};
}

std::string SpaceLayout::ComputeSignature() const {
    Matrix::RoomSummary Space;
    std::string Sig;
    if(m_Context->State.GetRoomSummary(m_SpaceId, Space)) {
        Sig += Space.Name;
        Sig += '\x1F';
        Sig += Space.Membership == Matrix::MembershipState::Join ? 'J' : '-';
        Sig += '\x1F';
    }
    for(const auto &Child : m_Context->State.GetSpaceChildren(m_SpaceId)) {
        Sig += Child.RoomId;
        Sig += '\x1F';
        Matrix::RoomSummary Summary;
        if(m_Context->State.GetRoomSummary(Child.RoomId, Summary)) {
            Sig += Matrix::MembershipToString(Summary.Membership);
            Sig += '\x1F';
            Sig += Summary.Name;
            Sig += '\x1F';
            Sig += Summary.AvatarMxc;
            Sig += '\x1F';
            Sig += Summary.LastMessagePreview;
            Sig += '\x1F';
            Sig += Summary.HasUnread() ? '1' : '0';
            Sig += std::to_string(Summary.NotificationCount);
            Sig += Summary.IsSpace ? 'S' : '-';
        } else if(const auto It = m_Hierarchy.find(Child.RoomId); It != m_Hierarchy.end()) {
            Sig += It->second.Name;
            Sig += '\x1F';
            Sig += It->second.AvatarMxc;
            Sig += '\x1F';
            Sig += It->second.RoomType;
            Sig += '\x1F';
            Sig += It->second.JoinRule;
        }
        Sig += '\x1F';
    }
    return Sig;
}

void SpaceLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();
    const std::string FocusChild = Focus < m_RowChildIds.size() ? m_RowChildIds[Focus] : "";

    // Header: live session summary first, hierarchy metadata as fallback for
    // a space joined moments ago (its state arrives with the next batch).
    Matrix::RoomSummary SpaceSummary;
    if(m_Context->State.GetRoomSummary(m_SpaceId, SpaceSummary)) {
        m_Header->SetText(Matrix::RoomDisplayName(SpaceSummary));
    } else if(const auto It = m_Hierarchy.find(m_SpaceId); It != m_Hierarchy.end() && !It->second.Name.empty()) {
        m_Header->SetText(It->second.Name);
    } else {
        m_Header->SetText(m_SpaceId);
    }

    const auto Children = m_Context->State.GetSpaceChildren(m_SpaceId);

    std::vector<Row::Ref> Rows;
    m_RowKinds.clear();
    m_RowChildIds.clear();
    auto Push = [&](RowKind Kind, const std::string &ChildId, const Row::Ref &RowElem) {
        Rows.push_back(RowElem);
        m_RowKinds.push_back(Kind);
        m_RowChildIds.push_back(ChildId);
    };

    if(!Children.empty()) {
        Push(RowKind::Info, "", std::make_shared<SectionHeaderRow>(Width, "Rooms"));
    }

    for(const auto &Child : Children) {
        Matrix::RoomSummary Summary;
        if(m_Context->State.GetRoomSummary(Child.RoomId, Summary)) {
            Push(Summary.IsSpace ? RowKind::Subspace : RowKind::Room, Child.RoomId,
                 std::make_shared<RoomRow>(Width, Summary, &m_Context->Media));
            continue;
        }

        // Child never joined: decorate from /hierarchy (name, avatar, type,
        // join rule); the room id stands in until the fetch lands.
        Matrix::RoomSummary Synth;
        Synth.RoomId = Child.RoomId;
        const auto It = m_Hierarchy.find(Child.RoomId);
        const bool Known = It != m_Hierarchy.end();
        bool Joinable = true;
        if(Known) {
            Synth.Name = It->second.Name;
            Synth.CanonicalAlias = It->second.CanonicalAlias;
            Synth.AvatarMxc = It->second.AvatarMxc;
            Synth.IsSpace = It->second.RoomType == "m.space";
            Synth.SpaceChildCount = static_cast<std::int32_t>(It->second.ChildCount);
            Joinable = JoinRuleAllows(It->second.JoinRule);
        }
        if(!Joinable) {
            Synth.LastMessagePreview = Synth.IsSpace ? "Space - invite only" : "Invite only";
        } else {
            Synth.LastMessagePreview = Synth.IsSpace ? "Space - press A to join" : "Not joined - press A to join";
        }
        Push(RowKind::NotJoined, Child.RoomId, std::make_shared<RoomRow>(Width, Synth, &m_Context->Media));
    }

    m_EmptyText->SetVisible(Children.empty());
    GetList()->SetRows(std::move(Rows));

    // Keep the focused child selected across rebuilds (rows shift).
    const auto Count = GetList()->GetRowCount();
    if(Count > 0) {
        std::size_t NewFocus = FocusChild.empty() ? std::min(Focus, Count - 1) : Count;
        if(!FocusChild.empty()) {
            for(std::size_t i = 0; i < m_RowChildIds.size(); i++) {
                if(m_RowChildIds[i] == FocusChild) {
                    NewFocus = i;
                    break;
                }
            }
        }
        if(NewFocus >= Count || m_RowChildIds[NewFocus].empty()) {
            for(std::size_t i = NewFocus < Count ? NewFocus : 0; i < Count; i++) {
                if(!m_RowChildIds[i].empty()) {
                    NewFocus = i;
                    break;
                }
            }
        }
        if(NewFocus < Count && !m_RowChildIds[NewFocus].empty()) {
            GetList()->SetFocusIndex(NewFocus);
        }
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

void SpaceLayout::FetchHierarchy() {
    if(m_HierarchyStarted) {
        return;
    }
    m_HierarchyStarted = true; // one shot: a failed fetch keeps the room ids
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string SpaceId = m_SpaceId;
    Core::JobQueue::Instance().Enqueue([this, Ctx, Alive, SpaceId]() {
        std::vector<Matrix::SpaceHierarchyRoom> Rooms;
        std::string From;
        for(int Page = 0; Page < 20; ++Page) { // guard against runaway tokens
            auto Resp = Ctx->Api.GetSpaceHierarchy(SpaceId, From, 200);
            if(!Resp.Ok) {
                break;
            }
            for(auto &Room : Resp.Value.Rooms) {
                Rooms.push_back(std::move(Room));
            }
            if(Resp.Value.NextBatch.empty()) {
                break;
            }
            From = Resp.Value.NextBatch;
        }
        Core::Dispatcher::Instance().Post([this, Alive, Rooms = std::move(Rooms)]() {
            if(!*Alive) {
                return;
            }
            for(const auto &Room : Rooms) {
                m_Hierarchy[Room.RoomId] = Room;
            }
            m_Dirty = true; // hierarchy is outside the session version
        });
    });
}

void SpaceLayout::JoinChild(const std::string &ChildId, const std::vector<std::string> &Via, bool TargetIsSpace) {
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::vector<std::string> Ancestors = m_Ancestors;
    const std::string SpaceId = m_SpaceId;
    Core::JobQueue::Instance().Enqueue([Ctx, Alive, ChildId, Via, TargetIsSpace, Ancestors, SpaceId]() {
        auto Joined = Ctx->Api.JoinRoom(ChildId, Via);
        Core::Dispatcher::Instance().Post([Ctx, Alive, Joined, TargetIsSpace, Ancestors, SpaceId]() {
            if(!*Alive) {
                return;
            }
            if(!Joined.Ok) {
                ShowError(Ctx->Nav.GetApp(), "Join failed", Joined.Error.ToDisplayString());
                return;
            }
            ShowToast(Ctx->Nav.GetApp(), "Joined");
            if(TargetIsSpace) {
                std::vector<std::string> ChildAncestors = Ancestors;
                ChildAncestors.push_back(SpaceId);
                Ctx->Nav.Push(SpaceLayout::New(Ctx, Joined.Value, std::move(ChildAncestors)));
            } else {
                Ctx->Nav.Push(RoomLayout::New(Ctx, Joined.Value));
            }
        });
    });
}

void SpaceLayout::OnRowActivated(std::size_t Index) {
    if(Index >= m_RowChildIds.size() || m_RowChildIds[Index].empty()) {
        return;
    }
    auto &App = m_Context->Nav.GetApp();
    auto Ctx = m_Context;
    const std::string ChildId = m_RowChildIds[Index];
    const RowKind Kind = m_RowKinds[Index];

    if(Kind == RowKind::Room || Kind == RowKind::Subspace) {
        Matrix::RoomSummary Summary;
        if(!m_Context->State.GetRoomSummary(ChildId, Summary)) {
            return;
        }
        if(Kind == RowKind::Room) {
            if(Summary.Membership == Matrix::MembershipState::Invite) {
                const std::string Name = Matrix::RoomDisplayName(Summary);
                if(!Confirm(App, "Room invitation", "\"" + Name + "\" invites you to join. Accept?", "Accept", "Decline")) {
                    return;
                }
                JoinChild(ChildId, ChildVia(ChildId), Summary.IsSpace);
                return;
            }
            Ctx->Nav.Push(RoomLayout::New(Ctx, ChildId));
            return;
        }
        // Subspace.
        if(WouldLoop(ChildId)) {
            ShowToast(App, "Space loop detected - not opening again");
            return;
        }
        if(Summary.Membership == Matrix::MembershipState::Invite) {
            const std::string Name = Matrix::RoomDisplayName(Summary);
            if(!Confirm(App, "Space invitation", "\"" + Name + "\" invites you to join. Accept?", "Accept", "Decline")) {
                return;
            }
            JoinChild(ChildId, ChildVia(ChildId), true);
            return;
        }
        std::vector<std::string> ChildAncestors = m_Ancestors;
        ChildAncestors.push_back(m_SpaceId);
        Ctx->Nav.Push(SpaceLayout::New(Ctx, ChildId, std::move(ChildAncestors)));
        return;
    }

    // NotJoined: the session has no state; /hierarchy decides type and access.
    const auto It = m_Hierarchy.find(ChildId);
    const bool Known = It != m_Hierarchy.end();
    const bool IsSpaceChild = Known && It->second.RoomType == "m.space";
    const bool Joinable = !Known || JoinRuleAllows(It->second.JoinRule);
    if(IsSpaceChild && WouldLoop(ChildId)) {
        ShowToast(App, "Space loop detected - not opening again");
        return;
    }
    if(!Joinable) {
        ShowMessage(App, "Invite only", "This room only accepts invitations.");
        return;
    }
    const std::string Name = (Known && !It->second.Name.empty()) ? It->second.Name : ChildId;
    if(!Confirm(App, IsSpaceChild ? "Join space" : "Join room", "Join \"" + Name + "\"?", "Join", "Cancel")) {
        return;
    }
    JoinChild(ChildId, ChildVia(ChildId), IsSpaceChild);
}

void SpaceLayout::RemoveChild(const std::string &ChildId) {
    auto &App = m_Context->Nav.GetApp();
    std::string Name = ChildId;
    Matrix::RoomSummary Summary;
    if(m_Context->State.GetRoomSummary(ChildId, Summary)) {
        Name = Matrix::RoomDisplayName(Summary);
    } else if(const auto It = m_Hierarchy.find(ChildId); It != m_Hierarchy.end() && !It->second.Name.empty()) {
        Name = It->second.Name;
    }
    if(!Confirm(App, "Remove from space", "Remove \"" + Name + "\" from this space?", "Remove")) {
        return;
    }
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string SpaceId = m_SpaceId;
    Core::JobQueue::Instance().Enqueue([Ctx, Alive, SpaceId, ChildId]() {
        // Empty content removes the child (spec §m.space.child).
        const nlohmann::json Empty = nlohmann::json::object();
        const auto Status = Ctx->Api.SendStateEvent(SpaceId, "m.space.child", ChildId, Empty);
        Core::Dispatcher::Instance().Post([Ctx, Alive, Status, SpaceId, ChildId]() {
            if(!*Alive) {
                return;
            }
            if(!Status.Ok) {
                ShowError(Ctx->Nav.GetApp(), "Not removed", Status.Error.ToDisplayString());
                return;
            }
            // Apply now; the sync echo of the same event is idempotent.
            Ctx->State.ApplyFetchedState(SpaceId, "m.space.child", nlohmann::json::object(), ChildId, Util::NowMs());
            ShowToast(Ctx->Nav.GetApp(), "Removed from space");
        });
    });
}

void SpaceLayout::ShowSpaceActions() {
    auto &App = m_Context->Nav.GetApp();
    Matrix::RoomSummary Summary;
    const bool HasSummary = m_Context->State.GetRoomSummary(m_SpaceId, Summary);
    const bool CanEdit = m_Context->State.CanSendState(m_SpaceId, "m.space.child");
    const std::string SpaceName = HasSummary ? Matrix::RoomDisplayName(Summary) : m_SpaceId;

    const auto Focus = GetList()->GetFocusIndex();
    const std::string FocusChild = Focus < m_RowChildIds.size() ? m_RowChildIds[Focus] : "";

    std::vector<std::string> Options;
    if(CanEdit && !FocusChild.empty()) {
        Options.push_back("Remove focused room");
    }
    if(HasSummary) {
        Options.push_back("Space settings");
    }
    if(HasSummary && Summary.Membership == Matrix::MembershipState::Join) {
        Options.push_back("Leave space");
    }
    if(Options.empty()) {
        ShowMessage(App, "Space actions", "No actions available for \"" + SpaceName + "\".");
        return;
    }
    const std::string Choice = ShowOptions(App, "Space actions", "Manage \"" + SpaceName + "\"", Options);
    if(Choice == "Remove focused room") {
        RemoveChild(FocusChild);
    } else if(Choice == "Space settings") {
        m_Context->Nav.Push(RoomDetailsLayout::New(m_Context, m_SpaceId));
    } else if(Choice == "Leave space") {
        if(!Confirm(App, "Leave space", "Leave \"" + SpaceName + "\"? Its rooms stay in your list.", "Leave")) {
            return;
        }
        auto Ctx = m_Context;
        auto Alive = m_Alive;
        const std::string SpaceId = m_SpaceId;
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, SpaceId]() {
            const auto Leave = Ctx->Api.LeaveRoom(SpaceId);
            (void)Ctx->Api.ForgetRoom(SpaceId); // best-effort: may already be gone
            Core::Dispatcher::Instance().Post([Ctx, Alive, Leave]() {
                if(!*Alive) {
                    return;
                }
                if(!Leave.Ok) {
                    ShowError(Ctx->Nav.GetApp(), "Leave failed", Leave.Error.ToDisplayString());
                    return;
                }
                Ctx->Nav.Pop(); // back to the room list
            });
        });
    }
}

} // namespace NxMatrix::Ui