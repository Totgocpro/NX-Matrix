#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace NxMatrix::Ui {

// Minimal SD card browser for picking local files (avatar and image uploads).
// The callback receives the absolute path; cancelling just pops the screen.
class FilePickerLayout : public pu::ui::Layout {
public:
    using PickedCallback = std::function<void(const std::string &Path)>;

    FilePickerLayout(std::shared_ptr<AppContext> Context, const std::vector<std::string> &Extensions, PickedCallback OnPicked);
    PU_SMART_CTOR(FilePickerLayout)

private:
    void RebuildRows();
    void OnEntryActivated(std::size_t Index);
    ListWidget *GetList() const;

    std::shared_ptr<AppContext> m_Context;
    std::vector<std::string> m_Extensions;
    PickedCallback m_OnPicked;
    std::string m_CurrentDir;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Header;
    pu::ui::elm::TextBlock::Ref m_Path;
    pu::ui::elm::TextBlock::Ref m_Hint;
    std::vector<std::pair<bool, std::string>> m_Entries;
    bool m_ReadFailed = false;
};

} // namespace NxMatrix::Ui