#pragma once
#include "NxMatrix/Ui/Elements/Rows.hpp"

#include <pu/Plutonium>

#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

namespace NxMatrix::Ui {

// Gamepad-navigable vertical list: D-pad/stick moves the focus (with key
// repeat), A activates the focused row. Only visible rows are rendered, and
// rows can have different heights (message bubbles).
class ListWidget : public pu::ui::elm::Element {
public:
    using ActivateCallback = std::function<void(std::size_t)>;
    using SelectionCallback = std::function<void(std::size_t)>;

    ListWidget(std::int32_t X, std::int32_t Y, std::int32_t Width, std::int32_t Height);
    // PU_SMART_CTOR already declares `using Ref = std::shared_ptr<ListWidget>;` plus New().
    PU_SMART_CTOR(ListWidget)

    void SetRows(std::vector<Row::Ref> Rows);
    void ClearRows();
    std::size_t GetRowCount() const;

    void SetOnActivate(ActivateCallback Callback);
    void SetOnSelectionChanged(SelectionCallback Callback);

    std::size_t GetFocusIndex() const;
    void SetFocusIndex(std::size_t Index);
    // False while another element (the room-list header toolbar) owns the
    // focus: input is ignored and the focused row loses its highlight.
    void SetInputEnabled(bool Enabled);
    // Whether a D-pad move in Delta (-1 up, +1 down) would land on a row.
    bool CanMoveFocus(int Delta) const;
    void ScrollToTop();
    void ScrollToBottom();
    bool IsAtBottom() const;

    std::int32_t GetX() override;
    std::int32_t GetY() override;
    std::int32_t GetWidth() override;
    std::int32_t GetHeight() override;
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;
    void OnInput(const std::uint64_t KeysDown, const std::uint64_t KeysUp, const std::uint64_t KeysHeld,
                 const pu::ui::TouchPoint TouchPos) override;

private:
    void MoveFocus(int Delta);
    void EnsureFocusVisible();
    std::size_t FindSelectable(std::size_t Start, int Delta) const;
    void SyncFocusState();
    std::int32_t GetRowOffset(std::size_t Index) const;
    std::int32_t GetContentHeight() const;
    std::int32_t GetMaxScroll() const;

    std::int32_t m_X;
    std::int32_t m_Y;
    std::int32_t m_Width;
    std::int32_t m_Height;
    std::vector<Row::Ref> m_Rows;
    std::size_t m_FocusIndex = 0;
    std::int32_t m_ScrollY = 0;
    ActivateCallback m_OnActivate;
    SelectionCallback m_OnSelectionChanged;
    std::chrono::steady_clock::time_point m_LastMove{};
    bool m_RepeatActive = false;
    bool m_RepeatFast = false;
    bool m_InputEnabled = true;
};

} // namespace NxMatrix::Ui