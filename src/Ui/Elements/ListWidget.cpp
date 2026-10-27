// Gamepad list: focus moves between selectable rows with key repeat, only
// rows crossing the viewport are rendered, and a scrollbar appears on
// overflow. Row heights vary, so offsets are recomputed from a running sum.
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include "NxMatrix/Ui/Theme.hpp"

#include <algorithm>

namespace NxMatrix::Ui {

namespace {

constexpr std::int32_t RowGap = 8;
constexpr std::int32_t ScrollbarWidth = 10;
constexpr std::chrono::milliseconds RepeatInitialDelay{280};
constexpr std::chrono::milliseconds RepeatFollowUpDelay{70};
constexpr std::int32_t BottomSnapEpsilon = 24;

} // namespace

ListWidget::ListWidget(std::int32_t X, std::int32_t Y, std::int32_t Width, std::int32_t Height)
    : m_X(X), m_Y(Y), m_Width(Width), m_Height(Height) {}

void ListWidget::SetRows(std::vector<Row::Ref> Rows) {
    m_Rows = std::move(Rows);
    if(m_Rows.empty()) {
        m_FocusIndex = 0;
        m_ScrollY = 0;
        return;
    }
    SetFocusIndex(std::min(m_FocusIndex, m_Rows.size() - 1));
    m_ScrollY = std::clamp(m_ScrollY, 0, GetMaxScroll());
}

void ListWidget::ClearRows() {
    m_Rows.clear();
    m_FocusIndex = 0;
    m_ScrollY = 0;
}

std::size_t ListWidget::GetRowCount() const {
    return m_Rows.size();
}

void ListWidget::SetInputEnabled(bool Enabled) {
    if(m_InputEnabled == Enabled) {
        return;
    }
    m_InputEnabled = Enabled;
    SyncFocusState();
}

bool ListWidget::CanMoveFocus(int Delta) const {
    return FindSelectable(m_FocusIndex, Delta) < m_Rows.size();
}

void ListWidget::SetOnActivate(ActivateCallback Callback) {
    m_OnActivate = std::move(Callback);
}

void ListWidget::SetOnSelectionChanged(SelectionCallback Callback) {
    m_OnSelectionChanged = std::move(Callback);
}

std::size_t ListWidget::GetFocusIndex() const {
    return m_FocusIndex;
}

void ListWidget::SetFocusIndex(std::size_t Index) {
    if(m_Rows.empty()) {
        m_FocusIndex = 0;
        SyncFocusState();
        return;
    }
    auto Target = std::min(Index, m_Rows.size() - 1);
    if(!m_Rows[Target]->IsSelectable()) {
        const auto Forward = FindSelectable(Target, 1);
        if(Forward < m_Rows.size()) {
            Target = Forward;
        } else {
            const auto Backward = FindSelectable(Target, -1);
            if(Backward < m_Rows.size()) {
                Target = Backward;
            }
        }
    }
    const auto Changed = Target != m_FocusIndex;
    m_FocusIndex = Target;
    SyncFocusState();
    EnsureFocusVisible();
    if(Changed && m_OnSelectionChanged) {
        m_OnSelectionChanged(m_FocusIndex);
    }
}

void ListWidget::ScrollToTop() {
    m_ScrollY = 0;
}

void ListWidget::ScrollToBottom() {
    m_ScrollY = GetMaxScroll();
}

bool ListWidget::IsAtBottom() const {
    return GetMaxScroll() - m_ScrollY <= BottomSnapEpsilon;
}

std::int32_t ListWidget::GetX() {
    return m_X;
}

std::int32_t ListWidget::GetY() {
    return m_Y;
}

std::int32_t ListWidget::GetWidth() {
    return m_Width;
}

std::int32_t ListWidget::GetHeight() {
    return m_Height;
}

std::int32_t ListWidget::GetRowOffset(std::size_t Index) const {
    std::int32_t Offset = 0;
    for(std::size_t I = 0; I < Index && I < m_Rows.size(); I++) {
        Offset += m_Rows[I]->GetHeight() + RowGap;
    }
    return Offset;
}

std::int32_t ListWidget::GetContentHeight() const {
    if(m_Rows.empty()) {
        return 0;
    }
    std::int32_t Total = 0;
    for(const auto &RowRef : m_Rows) {
        Total += RowRef->GetHeight() + RowGap;
    }
    return Total - RowGap;
}

std::int32_t ListWidget::GetMaxScroll() const {
    return std::max(0, GetContentHeight() - m_Height);
}

void ListWidget::SyncFocusState() {
    for(std::size_t I = 0; I < m_Rows.size(); I++) {
        m_Rows[I]->SetFocused(m_InputEnabled && I == m_FocusIndex);
    }
}

std::size_t ListWidget::FindSelectable(std::size_t Start, int Delta) const {
    if(m_Rows.empty()) {
        return m_Rows.size();
    }
    Start = std::min(Start, m_Rows.size() - 1);
    std::size_t I = Start;
    while(true) {
        if(Delta > 0) {
            if(I + static_cast<std::size_t>(Delta) >= m_Rows.size()) {
                return m_Rows.size();
            }
            I += static_cast<std::size_t>(Delta);
        } else if(Delta < 0) {
            const auto Step = static_cast<std::size_t>(-Delta);
            if(I < Step) {
                return m_Rows.size();
            }
            I -= Step;
        } else {
            return m_Rows[I]->IsSelectable() ? I : m_Rows.size();
        }
        if(m_Rows[I]->IsSelectable()) {
            return I;
        }
    }
}

void ListWidget::MoveFocus(int Delta) {
    if(m_Rows.empty()) {
        return;
    }
    const auto Target = FindSelectable(m_FocusIndex, Delta);
    if(Target >= m_Rows.size() || Target == m_FocusIndex) {
        return;
    }
    m_FocusIndex = Target;
    SyncFocusState();
    EnsureFocusVisible();
    if(m_OnSelectionChanged) {
        m_OnSelectionChanged(m_FocusIndex);
    }
}

void ListWidget::EnsureFocusVisible() {
    if(m_Rows.empty() || m_FocusIndex >= m_Rows.size()) {
        return;
    }
    const auto Top = GetRowOffset(m_FocusIndex);
    const auto Bottom = Top + m_Rows[m_FocusIndex]->GetHeight();
    if(Top < m_ScrollY) {
        m_ScrollY = Top;
    } else if(Bottom > m_ScrollY + m_Height) {
        m_ScrollY = Bottom - m_Height;
    }
    m_ScrollY = std::clamp(m_ScrollY, 0, GetMaxScroll());
}

void ListWidget::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    // Scissor to the viewport: partially visible rows (tall bubbles, the row
    // under focus) must not bleed over the header/composer or the gap.
    SDL_Rect ClipRect{X, Y, m_Width, m_Height};
    SDL_RenderSetClipRect(pu::ui::render::GetMainRenderer(), &ClipRect);
    std::int32_t Offset = 0;
    for(std::size_t I = 0; I < m_Rows.size(); I++) {
        auto &RowRef = m_Rows[I];
        const auto Height = RowRef->GetHeight();
        const auto Top = Offset - m_ScrollY;
        if(Top < m_Height && Top + Height > 0) {
            RowRef->OnRender(Drawer, X, Y + Top);
        }
        Offset += Height;
        if(I + 1 < m_Rows.size()) {
            Offset += RowGap;
        }
    }
    SDL_RenderSetClipRect(pu::ui::render::GetMainRenderer(), nullptr);

    const auto MaxScroll = GetMaxScroll();
    if(MaxScroll > 0) {
        const auto TrackX = X + m_Width - ScrollbarWidth;
        Drawer->RenderRectangleFill(Theme::Border, TrackX, Y, ScrollbarWidth, m_Height);
        const auto ContentH = GetContentHeight();
        const auto ThumbH = std::max(24, static_cast<std::int32_t>(static_cast<std::int64_t>(m_Height) * m_Height / ContentH));
        const auto ThumbY = Y + static_cast<std::int32_t>(static_cast<std::int64_t>(m_Height - ThumbH) * m_ScrollY / MaxScroll);
        Drawer->RenderRectangleFill(Theme::Accent, TrackX, ThumbY, ScrollbarWidth, ThumbH);
    }
}

void ListWidget::OnInput(const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t KeysHeld, const pu::ui::TouchPoint) {
    if(!m_InputEnabled) {
        return;
    }
    const bool DownNow = (KeysDown & HidNpadButton_AnyDown) != 0;
    const bool UpNow = (KeysDown & HidNpadButton_AnyUp) != 0;
    const bool DownHeld = (KeysHeld & HidNpadButton_AnyDown) != 0;
    const bool UpHeld = (KeysHeld & HidNpadButton_AnyUp) != 0;
    const bool AnyHeld = UpHeld || DownHeld;
    if(!AnyHeld) {
        m_RepeatActive = false;
        m_RepeatFast = false;
    }

    const auto Now = std::chrono::steady_clock::now();
    if(UpNow != DownNow) {
        MoveFocus(UpNow ? -1 : 1);
        m_LastMove = Now;
        m_RepeatActive = true;
        m_RepeatFast = false;
    } else if(m_RepeatActive && (UpHeld != DownHeld)) {
        const auto Delay = m_RepeatFast ? RepeatFollowUpDelay : RepeatInitialDelay;
        if(Now - m_LastMove >= Delay) {
            MoveFocus(UpHeld ? -1 : 1);
            m_LastMove = Now;
            m_RepeatFast = true;
        }
    }

    if((KeysDown & HidNpadButton_A) && m_OnActivate && m_FocusIndex < m_Rows.size() && m_Rows[m_FocusIndex]->IsSelectable()) {
        m_OnActivate(m_FocusIndex);
    }
}

} // namespace NxMatrix::Ui