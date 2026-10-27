#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <memory>
#include <string>

namespace NxMatrix::Ui {

// Boot screen when sealed data exists: enter the PIN/password to open the
// vault and restore the session. Escape hatches: log in with another account
// or wipe all stored data.
class UnlockLayout : public pu::ui::Layout {
public:
    explicit UnlockLayout(std::shared_ptr<AppContext> Context);
    ~UnlockLayout();
    PU_SMART_CTOR(UnlockLayout)

private:
    void RebuildRows();
    void OnUnlock();
    void OnOtherAccount();
    void OnWipe();
    void StartSession();

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Status;
    std::string m_Secret;
    int m_Attempts = 0;
    // Cleared by the destructor: unlock jobs posted back to the UI can detect
    // that this screen was replaced while the job was in flight.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui