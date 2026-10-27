#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <memory>
#include <string>

namespace NxMatrix::Ui {

// Sign-in and registration (mode switch). All text entry goes through the
// Horizon keyboard; the homeserver comes from AppContext::Account.
class LoginLayout : public pu::ui::Layout {
public:
    explicit LoginLayout(std::shared_ptr<AppContext> Context);
    ~LoginLayout();
    PU_SMART_CTOR(LoginLayout)

private:
    void RebuildRows();
    void OnFieldActivated(std::size_t Index);
    void OnSubmit();
    void OnAuthenticated(const Matrix::LoginResult &Result);

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Status;
    std::string m_UserName;
    std::string m_Password;
    bool m_IsRegisterMode = false;
    bool m_TermsAccepted = false;
    bool m_Busy = false;
    // Cleared by the destructor: sign-in results posted back to the UI can
    // detect that this screen was popped while the request was in flight.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui