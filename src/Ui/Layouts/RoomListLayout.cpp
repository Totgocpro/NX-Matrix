// Room list: grouped sections (invites/favourites/DMs/rooms) over the session
// summaries, and the header toolbar (settings, add/join, search) reached with
// the D-pad: Up from the first row enters it, Left/Right pick a tile, A fires.
#include "NxMatrix/Ui/Layouts/RoomListLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/CreateRoomLayout.hpp"
#include "NxMatrix/Ui/Layouts/RoomLayout.hpp"
#include "NxMatrix/Ui/Layouts/SearchLayout.hpp"
#include "NxMatrix/Ui/Layouts/SpaceLayout.hpp"
#include "NxMatrix/Ui/Layouts/SettingsLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

#include <algorithm>
#include <set>

namespace NxMatrix::Ui {

namespace {

// Header toolbar tiles, laid out left to right: settings, add/join (+), search.
constexpr std::int32_t TileSize = 56;
constexpr std::int32_t TileGap = 12;
constexpr std::int32_t ToolbarTileCount = 3;

} // namespace

RoomListLayout::RoomListLayout(std::shared_ptr<AppContext> Context) : m_Context(std::move(Context)) {
    this->SetBackgroundColor(Theme::Background);

    m_Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 24, "Chats");
    m_Header->SetFont(Theme::FontHeading);
    m_Header->SetColor(Theme::Accent);
    this->Add(m_Header);

    // Header toolbar, right-aligned next to the title: settings, add/join (+)
    // and search. Same tile size/gap so the row reads as one control.
    constexpr std::int32_t TileY = 20;
    const std::int32_t RightEdge = Theme::ScreenWidth - Theme::ScreenMargin;
    m_SearchTile = IconButton::New(RightEdge - TileSize, TileY, TileSize, "romfs:/search.png");
    m_AddTile = IconButton::New(RightEdge - 2 * TileSize - TileGap, TileY, TileSize, "romfs:/plus.png");
    m_SettingsTile = IconButton::New(RightEdge - 3 * TileSize - 2 * TileGap, TileY, TileSize, "romfs:/settings.png");
    this->Add(m_SettingsTile);
    this->Add(m_AddTile);
    this->Add(m_SearchTile);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);

    m_EmptyText = pu::ui::elm::TextBlock::New(0, Theme::ListTop + 160, "No rooms yet - press Up, then + to create or join one");
    m_EmptyText->SetFont(Theme::FontBody);
    m_EmptyText->SetColor(Theme::TextSecondary);
    const auto EmptyWidth = pu::ui::render::GetTextWidth(Theme::FontBody, m_EmptyText->GetText());
    m_EmptyText->SetX(std::max(0, (Theme::ScreenWidth - EmptyWidth) / 2));
    m_EmptyText->SetVisible(false);
    this->Add(m_EmptyText);

    m_Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56,
                                          "A Select  B Back  Up: toolbar  Left/Right: pick  Down: back");
    m_Hint->SetFont(Theme::FontTiny);
    m_Hint->SetColor(Theme::TextFaint);
    this->Add(m_Hint);

    GetList()->SetOnActivate([this](std::size_t Index) {
        if(Index >= m_RowRoomIds.size() || m_RowRoomIds[Index].empty()) {
            return;
        }
        Matrix::RoomSummary Summary;
        if(!m_Context->State.GetRoomSummary(m_RowRoomIds[Index], Summary)) {
            return;
        }
        if(Summary.Membership == Matrix::MembershipState::Invite) {
            OnInviteActivated(Index);
        } else if(Summary.IsSpace) {
            // Spaces group rooms; the spec discourages rendering their
            // timeline, so A opens the space's room list instead.
            m_Context->Nav.Push(SpaceLayout::New(m_Context, Summary.RoomId));
        } else {
            OnRoomActivated(Index);
        }
    });

    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        // + keeps its global shortcut to settings, in and out of the toolbar.
        if(KeysDown & HidNpadButton_Plus) {
            m_Context->Nav.Push(SettingsLayout::New(m_Context));
            return;
        }
        if(m_ToolbarFocus >= 0) {
            if(KeysDown & HidNpadButton_AnyLeft) {
                SetToolbarFocus((m_ToolbarFocus + ToolbarTileCount - 1) % ToolbarTileCount);
            } else if(KeysDown & HidNpadButton_AnyRight) {
                SetToolbarFocus((m_ToolbarFocus + 1) % ToolbarTileCount);
            } else if(KeysDown & HidNpadButton_A) {
                ActivateToolbarTile();
            } else if(KeysDown & (HidNpadButton_AnyDown | HidNpadButton_B)) {
                SetToolbarFocus(-1);
            }
            return;
        }
        // Up hands the focus to the toolbar only when the list has no row
        // above to move to (first selectable row, or an empty list).
        if((KeysDown & HidNpadButton_AnyUp) && !GetList()->CanMoveFocus(-1)) {
            SetToolbarFocus(0);
        }
    });

    // Rebuild only when the session moved AND the cheap fingerprint of the
    // rendered rows (ids, unread state, previews) changed.
    this->AddRenderCallback([this]() {
        const auto Version = m_Context->State.GetVersion();
        if(m_Dirty || Version != m_LastVersion) {
            m_LastVersion = Version;
            if(m_Dirty || ComputeSignature() != m_LastSignature) {
                RebuildRows();
            }
            m_Dirty = false;
        }
        BackfillRoomIcons();
    });

    RebuildRows();
}

ListWidget *RoomListLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

void RoomListLayout::Refresh() {
    // Force a rebuild on the next frame even if the signature looks unchanged.
    m_Dirty = true;
}

std::string RoomListLayout::ComputeSignature() const {
    std::string Sig;
    for(const auto &Summary : m_Context->State.GetRoomSummaries()) {
        Sig += Summary.RoomId;
        Sig += '\x1F';
        Sig += Matrix::MembershipToString(Summary.Membership);
        Sig += '\x1F';
        // The avatar is the one summary field with no other fingerprint: an
        // m.room.avatar change alone must still rebuild the rows.
        Sig += Summary.AvatarMxc;
        Sig += '\x1F';
        Sig += Summary.IsFavourite ? '1' : '0';
        Sig += Summary.IsDirect ? '1' : '0';
        Sig += Summary.IsLowPriority ? '1' : '0';
        Sig += Summary.IsSpace ? '1' : '0';
        Sig += std::to_string(Summary.SpaceChildCount);
        Sig += '\x1F';
        Sig += std::to_string(Summary.NotificationCount);
        Sig += '\x1F';
        Sig += std::to_string(Summary.HighlightCount);
        Sig += '\x1F';
        Sig += Summary.HasUnread() ? '1' : '0';
        Sig += '\x1F';
        Sig += std::to_string(Summary.LastActivityTs);
        Sig += '\x1F';
        Sig += Summary.LastMessagePreview;
        Sig += '\x1F';
    }
    return Sig;
}

void RoomListLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();
    const std::string FocusRoomId = Focus < m_RowRoomIds.size() ? m_RowRoomIds[Focus] : "";

    // Rooms grouped by their spaces: a child of a joined space is shown inside
    // that space's view, not again in the flat "Rooms" section. Favourites and
    // DMs are explicit cross-cutting pins and stay where the user put them.
    std::set<std::string> GroupedInSpace;
    for(const auto &Summary : m_Context->State.GetRoomSummaries()) {
        if(!Summary.IsSpace || Summary.Membership != Matrix::MembershipState::Join) {
            continue;
        }
        for(const auto &Child : m_Context->State.GetSpaceChildren(Summary.RoomId)) {
            GroupedInSpace.insert(Child.RoomId);
        }
    }

    std::vector<Matrix::RoomSummary> Invites;
    std::vector<Matrix::RoomSummary> Spaces;
    std::vector<Matrix::RoomSummary> Favourites;
    std::vector<Matrix::RoomSummary> DirectMessages;
    std::vector<Matrix::RoomSummary> Rooms;
    for(const auto &Summary : m_Context->State.GetRoomSummaries()) {
        if(Summary.Membership == Matrix::MembershipState::Invite) {
            Invites.push_back(Summary);
        } else if(Summary.IsSpace) {
            Spaces.push_back(Summary);
        } else if(Summary.IsFavourite) {
            Favourites.push_back(Summary);
        } else if(Summary.IsDirect) {
            DirectMessages.push_back(Summary);
        } else if(GroupedInSpace.count(Summary.RoomId) == 0) {
            Rooms.push_back(Summary);
        }
    }

    std::vector<Row::Ref> Rows;
    m_RowRoomIds.clear();
    auto PushSection = [&](const std::string &Title, const std::vector<Matrix::RoomSummary> &Group) {
        if(Group.empty()) {
            return;
        }
        Rows.push_back(std::make_shared<SectionHeaderRow>(Width, Title));
        m_RowRoomIds.push_back("");
        for(auto Room : Group) {
            if(Room.IsLowPriority) {
                Room.LastMessagePreview = "[low priority] " + Room.LastMessagePreview;
            }
            Rows.push_back(std::make_shared<RoomRow>(Width, Room, &m_Context->Media));
            m_RowRoomIds.push_back(Room.RoomId);
        }
    };
    PushSection("Invites", Invites);
    PushSection("Spaces", Spaces);
    PushSection("Favourites", Favourites);
    PushSection("Direct messages", DirectMessages);
    PushSection("Rooms", Rooms);

    m_EmptyText->SetVisible(m_RowRoomIds.empty());
    GetList()->SetRows(std::move(Rows));

    // Keep the focused room selected across rebuilds (rows shift with sections).
    const auto Count = GetList()->GetRowCount();
    if(Count > 0) {
        std::size_t NewFocus = FocusRoomId.empty() ? std::min(Focus, Count - 1) : Count;
        if(!FocusRoomId.empty()) {
            for(std::size_t i = 0; i < m_RowRoomIds.size(); i++) {
                if(m_RowRoomIds[i] == FocusRoomId) {
                    NewFocus = i;
                    break;
                }
            }
        }
        if(NewFocus >= Count || m_RowRoomIds[NewFocus].empty()) {
            for(std::size_t i = NewFocus < Count ? NewFocus : 0; i < Count; i++) {
                if(!m_RowRoomIds[i].empty()) {
                    NewFocus = i;
                    break;
                }
            }
        }
        if(NewFocus < Count && !m_RowRoomIds[NewFocus].empty()) {
            GetList()->SetFocusIndex(NewFocus);
        }
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

void RoomListLayout::SetToolbarFocus(std::int32_t Index) {
    m_ToolbarFocus = Index;
    // The list gives up its input and row highlight while a tile owns the focus.
    GetList()->SetInputEnabled(Index < 0);
    m_SettingsTile->SetFocused(Index == 0);
    m_AddTile->SetFocused(Index == 1);
    m_SearchTile->SetFocused(Index == 2);
}

void RoomListLayout::ActivateToolbarTile() {
    switch(m_ToolbarFocus) {
    case 0:
        m_Context->Nav.Push(SettingsLayout::New(m_Context));
        break;
    case 1:
        ShowRoomActions(); // create room / create space / join by id or alias
        break;
    case 2:
        m_Context->Nav.Push(SearchLayout::New(m_Context));
        break;
    default:
        break;
    }
}

void RoomListLayout::BackfillRoomIcons() {
    if(m_IconsBackfillDone) {
        return;
    }
    if(m_Context->State.GetRoomSummaries().empty()) {
        return; // rooms (and their state) only exist after the first sync batch
    }
    const auto Missing = m_Context->State.GetRoomIdsMissingState("m.room.avatar");
    m_IconsBackfillDone = true; // one shot: a failed fetch keeps the letter fallback
    auto Ctx = m_Context;
    for(const auto &RoomId : Missing) {
        Core::JobQueue::Instance().Enqueue([Ctx, RoomId]() {
            auto Fetched = Ctx->Api.GetStateEvent(RoomId, "m.room.avatar"); // 404 = room has no avatar
            Core::Dispatcher::Instance().Post([Ctx, RoomId, Fetched]() {
                if(Fetched.Ok) {
                    // Applies like a sync state block and bumps the version, so
                    // the rows rebuild and the icon is requested.
                    Ctx->State.ApplyFetchedState(RoomId, "m.room.avatar", Fetched.Value);
                }
            });
        });
    }
}

void RoomListLayout::OnRoomActivated(std::size_t Index) {
    if(Index >= m_RowRoomIds.size() || m_RowRoomIds[Index].empty()) {
        return;
    }
    m_Context->Nav.Push(RoomLayout::New(m_Context, m_RowRoomIds[Index]));
}

void RoomListLayout::OnInviteActivated(std::size_t Index) {
    if(Index >= m_RowRoomIds.size() || m_RowRoomIds[Index].empty()) {
        return;
    }
    const std::string RoomId = m_RowRoomIds[Index];
    Matrix::RoomSummary Summary;
    if(!m_Context->State.GetRoomSummary(RoomId, Summary)) {
        return;
    }
    const std::string Title = Matrix::RoomDisplayName(Summary);
    auto &App = m_Context->Nav.GetApp();
    const bool Accept = Confirm(App, "Room invitation", "\"" + Title + "\" invites you to join. Accept?", "Accept", "Decline");

    auto Ctx = m_Context;
    if(Accept) {
        Core::JobQueue::Instance().Enqueue([Ctx, RoomId, Title]() {
            auto Joined = Ctx->Api.JoinRoom(RoomId);
            Core::Dispatcher::Instance().Post([Ctx, Joined, Title]() {
                if(Joined.Ok) {
                    // The list refreshes itself on the next synced batch.
                    ShowToast(Ctx->Nav.GetApp(), "Joined " + Title);
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Join failed", Joined.Error.ToDisplayString());
                }
            });
        });
    } else {
        Core::JobQueue::Instance().Enqueue([Ctx, RoomId]() {
            auto Left = Ctx->Api.LeaveRoom(RoomId);
            Core::Dispatcher::Instance().Post([Ctx, Left]() {
                if(Left.Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Invitation declined");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Could not decline", Left.Error.ToDisplayString());
                }
            });
        });
    }
}

void RoomListLayout::ShowRoomActions() {
    auto &App = m_Context->Nav.GetApp();
    const std::string Choice = ShowOptions(App, "Room actions", "Create a room or a space, join another or open settings",
                                           {"Create room", "Create space", "Join by ID or alias", "Settings"});
    if(Choice == "Create room") {
        m_Context->Nav.Push(CreateRoomLayout::New(m_Context));
    } else if(Choice == "Create space") {
        m_Context->Nav.Push(CreateRoomLayout::New(m_Context, true));
    } else if(Choice == "Join by ID or alias") {
        std::string Input;
        if(!Keyboard::QueryText("Join room", "Room ID or #alias", Input)) {
            return;
        }
        Input = Util::Trim(Input);
        if(Input.empty()) {
            return;
        }
        auto Ctx = m_Context;
        Core::JobQueue::Instance().Enqueue([Ctx, Input]() {
            auto Joined = Ctx->Api.JoinRoom(Input);
            Core::Dispatcher::Instance().Post([Ctx, Joined]() {
                if(Joined.Ok) {
                    Ctx->Nav.Push(RoomLayout::New(Ctx, Joined.Value));
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Join failed", Joined.Error.ToDisplayString());
                }
            });
        });
    } else if(Choice == "Settings") {
        m_Context->Nav.Push(SettingsLayout::New(m_Context));
    }
}

} // namespace NxMatrix::Ui