#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Create-room form: name, topic, alias, invite list, private/public, direct
// chat and encryption toggles. SpaceMode creates an m.space room instead
// (no direct-chat/encryption rows: spaces group rooms, they are not chats).
class CreateRoomLayout : public pu::ui::Layout {
public:
    explicit CreateRoomLayout(std::shared_ptr<AppContext> Context, bool SpaceMode = false);
    PU_SMART_CTOR(CreateRoomLayout)
    ~CreateRoomLayout();

private:
    // Identifies what a row does (rows and headers share one index space).
    enum class RowKind : std::uint8_t {
        Name,
        Topic,
        Alias,
        InviteUsers,
        Public,
        Direct,
        Encrypted,
        Create,
    };

    void RebuildRows();
    void OnRowActivated(std::size_t Index);
    void OnCreate();
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Status;
    std::vector<RowKind> m_RowTags;
    std::string m_Name;
    std::string m_Topic;
    std::string m_Alias;
    std::string m_InviteList;
    bool m_IsPublic = false;
    bool m_IsDirect = false;
    bool m_IsEncrypted = true;
    bool m_SpaceMode = false;
    bool m_Busy = false;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a
    // popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui