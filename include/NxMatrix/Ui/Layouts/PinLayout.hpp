#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <memory>
#include <string>

namespace NxMatrix::Ui {

// Creates (first login) or changes the local PIN/password that seals all
// account data on the SD card.
class PinLayout : public pu::ui::Layout {
public:
    enum class Mode {
        Create,
        Change,
    };

    PinLayout(std::shared_ptr<AppContext> Context, Mode LayoutMode);
    PU_SMART_CTOR(PinLayout)

private:
    void RebuildRows();
    void OnFieldActivated(std::size_t Index);
    void OnConfirm();
    void Finish(const std::string &Secret);

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Status;
    Mode m_Mode;
    bool m_UsePassword = false;
    std::string m_Secret;
    std::string m_Confirm;
};

} // namespace NxMatrix::Ui