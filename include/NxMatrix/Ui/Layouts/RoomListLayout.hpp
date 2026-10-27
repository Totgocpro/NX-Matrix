#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/IconButton.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Main screen: room list (invites, favourites, DMs, rooms) with unread badges
// and previews, plus a header toolbar (settings, add/join, search). A opens the
// focused row; Up from the first row enters the toolbar, then Left/Right pick a
// tile and A activates it, Down goes back to the list (+ still opens settings).
class RoomListLayout : public pu::ui::Layout {
public:
    explicit RoomListLayout(std::shared_ptr<AppContext> Context);
    PU_SMART_CTOR(RoomListLayout)

    void Refresh();

private:
    void RebuildRows();
    void OnRoomActivated(std::size_t Index);
    void OnInviteActivated(std::size_t Index);
    void ShowRoomActions();
    // Header toolbar focus: -1 = room list owns the input, 0..2 = tile index
    // (settings, add/join, search). Toggles the list's input/highlight.
    void SetToolbarFocus(std::int32_t Index);
    void ActivateToolbarTile();
    // Fetches m.room.avatar for joined rooms whose state never carried it
    // (a resumed sync token only delivers changes). One shot per layout.
    void BackfillRoomIcons();
    ListWidget *GetList() const;
    std::string ComputeSignature() const;

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Header;
    pu::ui::elm::TextBlock::Ref m_Hint;
    pu::ui::elm::TextBlock::Ref m_EmptyText;
    IconButton::Ref m_SettingsTile;
    IconButton::Ref m_AddTile;
    IconButton::Ref m_SearchTile;
    std::int32_t m_ToolbarFocus = -1;
    std::vector<std::string> m_RowRoomIds;
    std::string m_LastSignature;
    std::uint64_t m_LastVersion = 0;
    bool m_Dirty = false;
    bool m_IconsBackfillDone = false;
};

} // namespace NxMatrix::Ui