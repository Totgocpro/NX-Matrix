// Server-side message search: Y opens the keyboard, hits are grouped by room
// and A opens the room a hit belongs to (when joined).
#include "NxMatrix/Ui/Layouts/SearchLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/RoomLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <cstdint>

namespace NxMatrix::Ui {

SearchLayout::SearchLayout(std::shared_ptr<AppContext> Context) : m_Context(std::move(Context)) {
    this->SetBackgroundColor(Theme::Background);

    auto Header = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "Search");
    Header->SetFont(Theme::FontHeading);
    Header->SetColor(Theme::Accent);
    this->Add(Header);

    m_Status = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 92, "Y: search messages");
    m_Status->SetFont(Theme::FontSmall);
    m_Status->SetColor(Theme::TextSecondary);
    this->Add(m_Status);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);
    GetList()->SetOnActivate([this](std::size_t Index) { OnResultActivated(Index); });

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "Y: search    A: open room");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        if(KeysDown & HidNpadButton_Y) {
            OnSearchActivated();
        }
    });

    // Room names in the group headers can change with sync; rebuild when the
    // version moved and the cheap result fingerprint changed.
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

SearchLayout::~SearchLayout() {
    *m_Alive = false;
}

void SearchLayout::Refresh() {
    RebuildRows();
}

ListWidget *SearchLayout::GetList() const {
    return static_cast<ListWidget *>(m_List.get());
}

std::string SearchLayout::ComputeSignature() const {
    std::string Sig = std::to_string(m_Results.Events.size());
    if(!m_Results.Events.empty()) {
        Sig += '\x1F';
        Sig += m_Results.Events.front().EventId;
        Sig += '\x1F';
        Sig += m_Results.Events.back().EventId;
    }
    return Sig;
}

void SearchLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    const auto Focus = GetList()->GetFocusIndex();
    const auto OwnUserId = m_Context->State.GetOwnUserId();

    std::vector<Row::Ref> Rows;
    m_RowEventIndex.clear();

    std::string PreviousRoom;
    for(std::size_t i = 0; i < m_Results.Events.size(); i++) {
        const auto &Event = m_Results.Events[i];
        if(Event.RoomId != PreviousRoom) {
            PreviousRoom = Event.RoomId;
            Matrix::RoomSummary Summary;
            auto RoomName = Event.RoomId;
            if(m_Context->State.GetRoomSummary(Event.RoomId, Summary) && !Summary.Name.empty()) {
                RoomName = Summary.Name;
            }
            Rows.push_back(std::make_shared<SectionHeaderRow>(Width, RoomName));
            m_RowEventIndex.push_back(SIZE_MAX);
        }
        Rows.push_back(std::make_shared<SimpleRow>(Width, Matrix::EventPreviewText(Event, OwnUserId), Util::FormatClock(Event.Timestamp)));
        m_RowEventIndex.push_back(i);
    }

    GetList()->SetRows(std::move(Rows));
    if(Focus < m_RowEventIndex.size() && m_RowEventIndex[Focus] != SIZE_MAX) {
        GetList()->SetFocusIndex(Focus);
    }

    m_LastSignature = ComputeSignature();
    m_LastVersion = m_Context->State.GetVersion();
}

void SearchLayout::OnSearchActivated() {
    std::string Query = m_Query;
    if(!Keyboard::QueryText("Search", "words to find", Query)) {
        return;
    }
    m_Query = Util::Trim(Query);

    if(m_Query.empty()) {
        m_Results = Matrix::SearchResult{};
        m_Status->SetColor(Theme::TextSecondary);
        m_Status->SetText("Y: search messages");
        RebuildRows();
        return;
    }

    m_Status->SetColor(Theme::TextFaint);
    m_Status->SetText("Searching...");

    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const std::string Term = m_Query;
    Core::JobQueue::Instance().Enqueue([Ctx, Alive, Term, this]() {
        auto Found = Ctx->Api.SearchMessages(Term, 30);
        Core::Dispatcher::Instance().Post([Ctx, Alive, Found, this]() {
            if(!*Alive) {
                return;
            }
            if(!Found.Ok) {
                m_Status->SetColor(Theme::Danger);
                m_Status->SetText("Search failed");
                ShowError(Ctx->Nav.GetApp(), "Search failed", Found.Error.ToDisplayString());
                return;
            }
            m_Results = Found.Value;
            const auto Count = m_Results.ResultCount != 0 ? m_Results.ResultCount : m_Results.Events.size();
            const auto Base = Count == 0 ? std::string("No results") : (std::to_string(Count) + " results");
            m_Status->SetColor(Theme::TextSecondary);
            m_Status->SetText(Base + "   \"" + m_Query + "\"");
            RebuildRows();
        });
    });
}

void SearchLayout::OnResultActivated(std::size_t Index) {
    if(Index >= m_RowEventIndex.size()) {
        return;
    }
    const auto EventIndex = m_RowEventIndex[Index];
    if(EventIndex == SIZE_MAX || EventIndex >= m_Results.Events.size()) {
        return;
    }
    const auto &Event = m_Results.Events[EventIndex];
    if(m_Context->State.HasRoom(Event.RoomId)) {
        m_Context->Nav.Push(RoomLayout::New(m_Context, Event.RoomId));
    } else {
        ShowError(m_Context->Nav.GetApp(), "Not in a room", "Join this room first to open it");
    }
}

} // namespace NxMatrix::Ui