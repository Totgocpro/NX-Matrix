// Room member roster: merges the state-event members with /joined_members,
// groups them by power level, name filter via Y, member actions via A.
#include "NxMatrix/Ui/Layouts/MemberListLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

#include <algorithm>

namespace NxMatrix::Ui {

namespace {

bool PassesFilter(const Matrix::RoomMember &Member, const std::string &NeedleLower) {
    if(NeedleLower.empty()) {
        return true;
    }
    return Util::ToLower(Member.DisplayName).find(NeedleLower) != std::string::npos ||
           Util::ToLower(Member.UserId).find(NeedleLower) != std::string::npos;
}

std::string SortName(const Matrix::RoomMember &Member) {
    return Util::ToLower(Member.DisplayName.empty() ? Member.UserId : Member.DisplayName);
}

} // namespace

MemberListLayout::MemberListLayout(std::shared_ptr<AppContext> Context, const std::string &RoomId)
    : m_Context(std::move(Context)), m_RoomId(RoomId) {
    this->SetBackgroundColor(Theme::Background);

    auto Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "Members");
    Header->SetFont(Theme::FontHeading);
    Header->SetColor(Theme::Accent);
    this->Add(Header);

    m_FilterLabel = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 92, "Filter: none");
    m_FilterLabel->SetFont(Theme::FontSmall);
    m_FilterLabel->SetColor(Theme::TextSecondary);
    this->Add(m_FilterLabel);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnMemberActivated(Index); });

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "Y: filter    A: member actions");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        if(KeysDown & HidNpadButton_Y) {
            OnFilterActivated();
        }
    });

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
    FetchJoinedMembers();
}

MemberListLayout::~MemberListLayout() {
    *m_Alive = false;
}

void MemberListLayout::Refresh() {
    RebuildRows();
    FetchJoinedMembers();
}

ListWidget *MemberListLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

void MemberListLayout::FetchJoinedMembers() {
    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string Room = m_RoomId;
    Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, this]() {
        auto Fetched = Ctx->Api.GetJoinedMembers(Room);
        Core::Dispatcher::Instance().Post([Ctx, Alive, Fetched, this]() {
            if(!*Alive) {
                return;
            }
            if(Fetched.Ok) {
                m_Fetched = Fetched.Value;
                RebuildRows();
            }
        });
    });
}

std::string MemberListLayout::ComputeSignature() const {
    std::string Sig = m_Filter;
    const auto Members = m_Context->State.GetMembers(m_RoomId);
    Sig += '\x1F' + std::to_string(Members.size()) + '\x1F' + std::to_string(m_Fetched.size());
    for(const auto &Member : Members) {
        Sig += Member.UserId;
        Sig += '\x1E';
        Sig += Member.DisplayName;
        Sig += '\x1E';
        Sig += std::to_string(Member.PowerLevel);
        Sig += '\x1E';
        Sig += std::to_string(static_cast<int>(Member.Membership));
        Sig += '\x1E';
    }
    for(const auto &Pair : m_Fetched) {
        Sig += Pair.first;
        Sig += '\x1E';
        Sig += Pair.second.DisplayName;
        Sig += '\x1E';
        Sig += std::to_string(Pair.second.PowerLevel);
        Sig += '\x1E';
    }
    return Sig;
}

void MemberListLayout::RebuildRows() {
    m_FilterLabel->SetText("Filter: " + (m_Filter.empty() ? std::string("none") : m_Filter));

    const auto Levels = m_Context->State.GetPowerLevels(m_RoomId);

    // Merge state-event members with the server's joined roster (the latter
    // carries fresher display names and avatars).
    std::map<std::string, Matrix::RoomMember> Merged;
    for(const auto &Member : m_Context->State.GetMembers(m_RoomId)) {
        Merged[Member.UserId] = Member;
    }
    for(const auto &Pair : m_Fetched) {
        auto It = Merged.find(Pair.first);
        if(It != Merged.end()) {
            if(!Pair.second.DisplayName.empty()) {
                It->second.DisplayName = Pair.second.DisplayName;
            }
            if(!Pair.second.AvatarMxc.empty()) {
                It->second.AvatarMxc = Pair.second.AvatarMxc;
            }
        } else {
            Matrix::RoomMember Member = Pair.second;
            Member.UserId = Pair.first;
            Member.Membership = Matrix::MembershipState::Join;
            Merged[Member.UserId] = Member;
        }
    }
    // Members known from state events carry no level of their own: the level
    // always comes from the room's power levels (room v12+ creators included).
    for(auto &Pair : Merged) {
        Pair.second.PowerLevel = Levels.GetUserLevel(Pair.first);
    }

    const auto NeedleLower = Util::ToLower(m_Filter);
    std::vector<Matrix::RoomMember> Admins;
    std::vector<Matrix::RoomMember> Joined;
    std::vector<Matrix::RoomMember> Invited;
    for(const auto &Pair : Merged) {
        const auto &Member = Pair.second;
        if(!PassesFilter(Member, NeedleLower)) {
            continue;
        }
        if(Member.Membership == Matrix::MembershipState::Invite) {
            Invited.push_back(Member);
        } else if(Member.Membership == Matrix::MembershipState::Join) {
            if(Member.PowerLevel >= 50) {
                Admins.push_back(Member);
            } else {
                Joined.push_back(Member);
            }
        }
    }

    auto ByName = [](const Matrix::RoomMember &A, const Matrix::RoomMember &B) {
        const auto NameA = SortName(A);
        const auto NameB = SortName(B);
        if(NameA != NameB) {
            return NameA < NameB;
        }
        return A.UserId < B.UserId;
    };
    std::sort(Admins.begin(), Admins.end(), ByName);
    std::sort(Joined.begin(), Joined.end(), ByName);
    std::sort(Invited.begin(), Invited.end(), ByName);

    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();

    std::vector<Row::Ref> Rows;
    m_RowMembers.clear();

    auto PushHeader = [&](const std::string &Title) {
        Rows.push_back(std::make_shared<SectionHeaderRow>(Width, Title));
        m_RowMembers.push_back(Matrix::RoomMember{});
    };
    auto PushMembers = [&](const std::vector<Matrix::RoomMember> &Group) {
        for(const auto &Member : Group) {
            Rows.push_back(std::make_shared<MemberRow>(Width, Member, &m_Context->Media));
            m_RowMembers.push_back(Member);
        }
    };

    if(!Admins.empty()) {
        PushHeader("Owners/Admins");
        PushMembers(Admins);
    }
    if(!Joined.empty()) {
        PushHeader("Members");
        PushMembers(Joined);
    }
    if(!Invited.empty()) {
        PushHeader("Invited");
        PushMembers(Invited);
    }

    GetList()->SetRows(std::move(Rows));
    if(Focus < m_RowMembers.size() && !m_RowMembers[Focus].UserId.empty()) {
        GetList()->SetFocusIndex(Focus);
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

void MemberListLayout::OnFilterActivated() {
    if(!Keyboard::QueryText("Filter", "name or @id", m_Filter)) {
        return;
    }
    // Empty input clears the filter (QueryText only runs on success).
    RebuildRows();
}

void MemberListLayout::OnMemberActivated(std::size_t Index) {
    if(Index >= m_RowMembers.size()) {
        return;
    }
    const auto Member = m_RowMembers[Index];
    if(Member.UserId.empty()) {
        return;
    }

    auto &App = m_Context->Nav.GetApp();
    const auto OwnUserId = m_Context->State.GetOwnUserId();
    const bool IsSelf = Member.UserId == OwnUserId;
    const auto Levels = m_Context->State.GetPowerLevels(m_RoomId);
    const auto OwnLevel = Levels.GetUserLevel(OwnUserId);

    std::vector<std::string> Options{"Show profile"};
    if(!IsSelf) {
        if(Member.Membership != Matrix::MembershipState::Join && OwnLevel >= Levels.Invite) {
            Options.push_back("Invite to room");
        }
        if(Member.Membership == Matrix::MembershipState::Join && OwnLevel >= Levels.Kick) {
            Options.push_back("Kick");
        }
        if(OwnLevel >= Levels.Ban) {
            Options.push_back("Ban");
        }
    }

    const auto Title = Member.DisplayName.empty() ? Member.UserId : Member.DisplayName;
    const auto Choice = ShowOptions(App, Title, Member.DisplayName.empty() ? std::string() : Member.UserId, Options);
    if(Choice.empty()) {
        return;
    }
    if(Choice == "Show profile") {
        const std::string LevelText = Member.PowerLevel == Matrix::PowerLevels::InfiniteLevel
                                          ? std::string("infinite (room creator)")
                                          : std::to_string(Member.PowerLevel);
        ShowMessage(App, "Profile",
                    "Display name: " + (Member.DisplayName.empty() ? std::string("-") : Member.DisplayName) + //
                        "\nUser id: " + Member.UserId + "\nPower level: " + LevelText);
        return;
    }

    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string Room = m_RoomId;
    const std::string UserId = Member.UserId;

    if(Choice == "Invite to room") {
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
    } else if(Choice == "Kick") {
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, UserId]() {
            const auto Status = Ctx->Api.KickUser(Room, UserId);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status]() {
                if(!*Alive) {
                    return;
                }
                if(Status.Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Member kicked");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Kick failed", Status.Error.ToDisplayString());
                }
            });
        });
    } else if(Choice == "Ban") {
        Core::JobQueue::Instance().Enqueue([Ctx, Alive, Room, UserId]() {
            const auto Status = Ctx->Api.BanUser(Room, UserId);
            Core::Dispatcher::Instance().Post([Ctx, Alive, Status]() {
                if(!*Alive) {
                    return;
                }
                if(Status.Ok) {
                    ShowToast(Ctx->Nav.GetApp(), "Member banned");
                } else {
                    ShowError(Ctx->Nav.GetApp(), "Ban failed", Status.Error.ToDisplayString());
                }
            });
        });
    }
}

} // namespace NxMatrix::Ui