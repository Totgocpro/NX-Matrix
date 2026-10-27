#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Server-side message search (POST /search): type a term, browse the hits,
// open the room a hit belongs to.
class SearchLayout : public pu::ui::Layout {
public:
    explicit SearchLayout(std::shared_ptr<AppContext> Context);
    PU_SMART_CTOR(SearchLayout)
    ~SearchLayout();

    void Refresh();

private:
    void RebuildRows();
    void OnSearchActivated();
    void OnResultActivated(std::size_t Index);
    // Cheap fingerprint of the rendered hits (invalidation guard).
    std::string ComputeSignature() const;
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Status;
    std::string m_Query;
    Matrix::SearchResult m_Results;
    // Row -> index into m_Results.Events (section headers carry SIZE_MAX).
    std::vector<std::size_t> m_RowEventIndex;
    std::uint64_t m_LastVersion = 0;
    std::string m_LastSignature;
    // Flipped by the destructor so queued Dispatcher callbacks never touch a
    // popped layout.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui