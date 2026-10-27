#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Space view: the ordered m.space.child rooms of a space-room (spec v1.19
// §Spaces). A opens a joined room, joins a not-yet-joined child or descends
// into a subspace; X manages the space (settings, remove room, leave).
// Spaces are never opened as timelines: the spec discourages messages in
// space-rooms and clients are not expected to render them.
class SpaceLayout : public pu::ui::Layout {
public:
    // Ancestors = the spaces already open above this one. m.space.child
    // graphs can contain loops (spec: break them, never traverse twice).
    SpaceLayout(std::shared_ptr<AppContext> Context, const std::string &SpaceId, std::vector<std::string> Ancestors = {});
    PU_SMART_CTOR(SpaceLayout)
    ~SpaceLayout();

private:
    // What a row does; rows and section headers share one index space.
    enum class RowKind : std::uint8_t {
        Info,      // non-selectable section header
        Room,      // joined or invited child room
        Subspace,  // child space present in the session
        NotJoined, // child without local state, decorated from /hierarchy
    };

    void RebuildRows();
    void OnRowActivated(std::size_t Index);
    void ShowSpaceActions();
    void RemoveChild(const std::string &ChildId);
    // Joins ChildId through Via (from the space's m.space.child), then opens
    // it as a space or a room view.
    void JoinChild(const std::string &ChildId, const std::vector<std::string> &Via, bool TargetIsSpace);
    // One paginated GET /hierarchy: fills names/avatars/join rules for
    // children the session does not know (not joined yet).
    void FetchHierarchy();
    bool WouldLoop(const std::string &ChildId) const;
    std::vector<std::string> ChildVia(const std::string &ChildId) const;
    std::string ComputeSignature() const;
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    std::string m_SpaceId;
    std::vector<std::string> m_Ancestors;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Header;
    pu::ui::elm::TextBlock::Ref m_EmptyText;
    std::vector<RowKind> m_RowKinds;
    std::vector<std::string> m_RowChildIds; // parallel to rows, "" = not selectable
    // /hierarchy decoration for unknown children. UI thread only: filled by a
    // Dispatcher post, read by RebuildRows.
    std::map<std::string, Matrix::SpaceHierarchyRoom> m_Hierarchy;
    std::string m_LastSignature;
    std::uint64_t m_LastVersion = 0;
    bool m_Dirty = false;
    bool m_HierarchyStarted = false;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a
    // popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui