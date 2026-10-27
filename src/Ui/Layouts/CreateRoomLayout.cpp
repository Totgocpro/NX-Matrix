// Create-room form: text fields via the Horizon keyboard, three toggles and
// the create action with validation + busy guard.
#include "NxMatrix/Ui/Layouts/CreateRoomLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

#include <algorithm>

namespace NxMatrix::Ui {

namespace {

bool IsAliasChar(char Ch) {
    return (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '.' || Ch == '_' || Ch == '~' || Ch == '/' || Ch == '+' ||
           Ch == '-';
}

} // namespace

CreateRoomLayout::CreateRoomLayout(std::shared_ptr<AppContext> Context, bool SpaceMode)
    : m_Context(std::move(Context)), m_SpaceMode(SpaceMode) {
    this->SetBackgroundColor(Theme::Background);

    auto Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, m_SpaceMode ? "Create space" : "Create room");
    Header->SetFont(Theme::FontHeading);
    Header->SetColor(Theme::Accent);
    this->Add(Header);

    m_Status = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 92, "");
    m_Status->SetFont(Theme::FontSmall);
    m_Status->SetColor(Theme::Danger);
    this->Add(m_Status);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnRowActivated(Index); });

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A: edit or toggle");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    RebuildRows();
}

CreateRoomLayout::~CreateRoomLayout() {
    *m_Alive = false;
}

ListWidget *CreateRoomLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

void CreateRoomLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();

    std::vector<Row::Ref> Rows;
    m_RowTags.clear();

    auto Push = [&](RowKind Kind, const Row::Ref &RowElem) {
        Rows.push_back(RowElem);
        m_RowTags.push_back(Kind);
    };

    Push(RowKind::Name, std::make_shared<TextInputRow>(Width, "Name", m_Name, m_SpaceMode ? "Space name" : "Room name"));
    Push(RowKind::Topic, std::make_shared<TextInputRow>(Width, "Topic", m_Topic, "Optional"));
    Push(RowKind::Alias, std::make_shared<TextInputRow>(Width, "Room alias", m_Alias, "Public address under #alias:server"));
    Push(RowKind::InviteUsers, std::make_shared<TextInputRow>(Width, "Invite users", m_InviteList, "Optional, comma-separated"));
    Push(RowKind::Public, std::make_shared<ToggleRow>(Width, m_SpaceMode ? "Public space" : "Public room", m_IsPublic));
    if(!m_SpaceMode) {
        // A space groups rooms: direct chat and message encryption do not
        // apply to its (discouraged) timeline.
        Push(RowKind::Direct, std::make_shared<ToggleRow>(Width, "Direct chat", m_IsDirect));
        Push(RowKind::Encrypted, std::make_shared<ToggleRow>(Width, "Encrypt messages", m_IsEncrypted));
    }
    Push(RowKind::Create, std::make_shared<SimpleRow>(Width, "Create"));

    GetList()->SetRows(std::move(Rows));
    if(Focus < m_RowTags.size()) {
        GetList()->SetFocusIndex(Focus);
    }
}

void CreateRoomLayout::OnRowActivated(std::size_t Index) {
    if(Index >= m_RowTags.size()) {
        return;
    }

    switch(m_RowTags[Index]) {
    case RowKind::Name: {
        std::string Value = m_Name;
        if(Keyboard::QueryText("Room name", "Name of the room", Value)) {
            m_Name = Value;
            RebuildRows();
        }
        break;
    }
    case RowKind::Topic: {
        std::string Value = m_Topic;
        if(Keyboard::QueryLongText("Topic", "Room topic", Value)) {
            m_Topic = Value;
            RebuildRows();
        }
        break;
    }
    case RowKind::Alias: {
        std::string Value = m_Alias;
        if(Keyboard::QueryText("Room alias", "Public address under #alias:server", Value)) {
            if(!Value.empty() && Value.front() == '#') {
                Value = Value.substr(1);
            }
            m_Alias = Value;
            RebuildRows();
        }
        break;
    }
    case RowKind::InviteUsers: {
        std::string Value = m_InviteList;
        if(Keyboard::QueryText("Invite users", "@user:server.org, @other:server.org", Value)) {
            m_InviteList = Value;
            RebuildRows();
        }
        break;
    }
    case RowKind::Public:
        m_IsPublic = !m_IsPublic;
        RebuildRows();
        break;
    case RowKind::Direct:
        m_IsDirect = !m_IsDirect;
        RebuildRows();
        break;
    case RowKind::Encrypted:
        m_IsEncrypted = !m_IsEncrypted;
        RebuildRows();
        break;
    case RowKind::Create:
        OnCreate();
        break;
    }
}

void CreateRoomLayout::OnCreate() {
    if(m_Busy) {
        return;
    }

    const auto Name = Util::Trim(m_Name);
    auto Alias = m_Alias;
    if(!Alias.empty() && Alias.front() == '#') {
        Alias = Alias.substr(1);
    }
    if(Name.empty() && Alias.empty()) {
        m_Status->SetColor(Theme::Danger);
        m_Status->SetText("Enter a room name or an alias");
        return;
    }
    if(!Alias.empty() && !std::all_of(Alias.begin(), Alias.end(), IsAliasChar)) {
        m_Status->SetColor(Theme::Danger);
        m_Status->SetText("Alias may only use a-z 0-9 . _ ~ / + -");
        return;
    }

    std::vector<std::string> InviteIds;
    for(const auto &RawId : Util::Split(m_InviteList, ',')) {
        const auto UserId = Util::Trim(RawId);
        if(UserId.empty()) {
            continue;
        }
        if(!Util::StartsWith(UserId, "@") || UserId.find(':') == std::string::npos) {
            m_Status->SetColor(Theme::Danger);
            m_Status->SetText("Invalid invite address: " + UserId);
            return;
        }
        InviteIds.push_back(UserId);
    }

    Matrix::RoomCreateParams Params;
    Params.Name = Name;
    Params.Topic = m_Topic;
    Params.RoomAlias = Alias;
    Params.InviteUserIds = std::move(InviteIds);
    Params.IsPublic = m_IsPublic;
    Params.IsDirect = m_IsDirect && !m_SpaceMode;
    Params.IsEncrypted = m_IsEncrypted && !m_SpaceMode;
    Params.IsSpace = m_SpaceMode;

    m_Busy = true;
    m_Status->SetColor(Theme::TextFaint);
    m_Status->SetText(m_SpaceMode ? "Creating space..." : "Creating room...");

    auto Ctx = m_Context;
    auto Alive = m_Alive;
    Core::JobQueue::Instance().Enqueue([Ctx, Alive, Params, this]() {
        auto Created = Ctx->Api.CreateRoom(Params);
        Core::Dispatcher::Instance().Post([Ctx, Alive, Created, this]() mutable {
            if(!*Alive) {
                return;
            }
            m_Busy = false;
            if(!Created.Ok) {
                m_Status->SetText("");
                ShowError(Ctx->Nav.GetApp(), m_SpaceMode ? "Space creation failed" : "Create room failed",
                          Created.Error.ToDisplayString());
                return;
            }
            m_Status->SetText("");
            ShowToast(Ctx->Nav.GetApp(), m_SpaceMode ? "Space created" : "Room created");
            // Back to the room list; sync adds the new room shortly.
            Ctx->Nav.Pop();
        });
    });
}

} // namespace NxMatrix::Ui