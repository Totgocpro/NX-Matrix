#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Room timeline: paginated message list, composer (Horizon keyboard), typing
// indicator and per-message actions (reply, react, edit, redact).
class RoomLayout : public pu::ui::Layout {
public:
    RoomLayout(std::shared_ptr<AppContext> Context, const std::string &RoomId);
    PU_SMART_CTOR(RoomLayout)
    ~RoomLayout();

    void Refresh();

private:
    void RebuildRows();
    void OnComposerActivated();
    void OnMessageActivated(std::size_t Index);
    void SendMessage(const std::string &Body, const std::string &ReplyToEventId, const std::string &EditEventId);
    void LoadOlder();
    void SendReadMarker();
    void UpdateStatusLine();
    void UpdateChrome();
    ListWidget *GetList() const;
    std::string ComputeSignature() const;
    std::size_t ComputeFloor(std::size_t TimelineSize) const;
    std::string EffectiveBody(const std::string &EventId) const;
    std::string MemberName(const std::string &UserId) const;
    std::string StateLine(const Matrix::Event &Event) const;

    std::shared_ptr<AppContext> m_Context;
    std::string m_RoomId;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Header;
    pu::ui::elm::TextBlock::Ref m_Composer;
    pu::ui::elm::TextBlock::Ref m_StatusLine;
    pu::ui::elm::TextBlock::Ref m_Meta;
    pu::ui::elm::TextBlock::Ref m_Typing;
    std::vector<std::string> m_RowEventIds;
    std::string m_ReplyToEventId;
    std::string m_ReplySenderName;
    std::string m_EditEventId;
    std::string m_LastMarkedEventId;
    std::string m_LastSignature;
    std::string m_LastHeader;
    std::string m_LastMeta;
    std::string m_LastTyping;
    std::string m_LastStatus;
    std::string m_LastComposerKey;
    std::string m_LastComposerText;
    std::string m_ComposerPreview;
    std::string m_TransientStatus;
    std::string m_PrevBatch;
    std::size_t m_RenderFloor = 0;
    bool m_FloorSet = false;
    bool m_PaginationDone = false;
    std::uint64_t m_LastVersion = 0;
    bool m_LoadingOlder = false;
    bool m_StickToBottom = true;
    bool m_Dirty = false;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui