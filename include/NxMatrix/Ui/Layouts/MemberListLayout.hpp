#pragma once
#include "NxMatrix/Matrix/Models.hpp"
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Room members with a name filter; A opens member actions (profile, message,
// invite to another room is out of scope, kick/ban/unban when permitted).
class MemberListLayout : public pu::ui::Layout {
public:
    MemberListLayout(std::shared_ptr<AppContext> Context, const std::string &RoomId);
    PU_SMART_CTOR(MemberListLayout)
    ~MemberListLayout();

    void Refresh();

private:
    void RebuildRows();
    void OnMemberActivated(std::size_t Index);
    void OnFilterActivated();
    // Fetches /joined_members off-thread and rebuilds on arrival.
    void FetchJoinedMembers();
    // Cheap fingerprint of everything the rows display (invalidation guard).
    std::string ComputeSignature() const;
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    std::string m_RoomId;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_FilterLabel;
    std::string m_Filter;
    // Server-side joined members merged over the state-event roster.
    std::map<std::string, Matrix::RoomMember> m_Fetched;
    // Row -> member mapping (section headers carry an empty UserId).
    std::vector<Matrix::RoomMember> m_RowMembers;
    std::uint64_t m_LastVersion = 0;
    std::string m_LastSignature;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a
    // popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui