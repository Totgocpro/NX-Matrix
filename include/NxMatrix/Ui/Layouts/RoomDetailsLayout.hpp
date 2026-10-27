#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Room details: topic, members, favourite/low-priority tags, encryption info,
// name/topic editing (when allowed by power levels), leave and forget.
class RoomDetailsLayout : public pu::ui::Layout {
public:
    RoomDetailsLayout(std::shared_ptr<AppContext> Context, const std::string &RoomId);
    PU_SMART_CTOR(RoomDetailsLayout)
    ~RoomDetailsLayout();

    void Refresh();

private:
    // Identifies what a row does (rows and headers share one index space).
    enum class RowKind : std::uint8_t {
        Header,
        Topic,
        Name,
        Members,
        Invite,
        Favourite,
        LowPriority,
        Encryption,
        AddToSpace, // link this room into a joined space (m.space.child)
        Leave,
    };

    void RebuildRows();
    void OnRowActivated(std::size_t Index);
    // Joined spaces that do not contain this room yet and accept m.space.child
    // from us; empty = the AddToSpace row is not offered.
    std::vector<std::string> EligibleSpaces(const Matrix::RoomSummary &Summary) const;
    // Cheap fingerprint of everything the rows display (invalidation guard).
    std::string ComputeSignature() const;
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    std::string m_RoomId;
    ListWidget::Ref m_List;
    std::vector<RowKind> m_RowTags;
    std::uint64_t m_LastVersion = 0;
    std::string m_LastSignature;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a
    // popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui