// Sign-in / registration screen: fields go through the Horizon keyboard,
// authentication runs on a worker and hands off to the PIN creation screen.
#include "NxMatrix/Ui/Layouts/LoginLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/PinLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

namespace NxMatrix::Ui {

namespace {

void SetStatusText(pu::ui::elm::TextBlock::Ref &Status, const std::string &Text, const pu::ui::Color &Color) {
    Status->SetColor(Color);
    Status->SetText(Text);
}

} // namespace

LoginLayout::LoginLayout(std::shared_ptr<AppContext> Context) : m_Context(std::move(Context)) {
    this->SetBackgroundColor(Theme::Background);

    m_Status = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 88, "");
    m_Status->SetFont(Theme::FontSmall);
    m_Status->SetColor(Theme::TextSecondary);
    this->Add(m_Status);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);

    m_List->SetOnActivate([this](std::size_t Index) { OnFieldActivated(Index); });

    RebuildRows();
}

LoginLayout::~LoginLayout() {
    *m_Alive = false;
}

void LoginLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    std::vector<Row::Ref> Rows;
    Rows.push_back(std::make_shared<ToggleRow>(Width, "Create an account instead", m_IsRegisterMode));
    if(m_IsRegisterMode) {
        Rows.push_back(std::make_shared<ToggleRow>(Width, "I accept the homeserver terms", m_TermsAccepted));
    }
    Rows.push_back(std::make_shared<SimpleRow>(Width, "Homeserver", m_Context->Account.HomeserverUrl));
    Rows.push_back(std::make_shared<TextInputRow>(Width, "Username", m_UserName));
    Rows.push_back(std::make_shared<TextInputRow>(Width, "Password", std::string(m_Password.size(), '*'), "Tap A to enter"));
    Rows.push_back(std::make_shared<SimpleRow>(Width, m_IsRegisterMode ? "Create account" : "Sign in"));
    m_List->SetRows(std::move(Rows));
}

void LoginLayout::OnFieldActivated(std::size_t Index) {
    if(Index == 0) {
        m_IsRegisterMode = !m_IsRegisterMode;
        RebuildRows();
        return;
    }
    const std::size_t Base = m_IsRegisterMode ? 2 : 1;
    if(m_IsRegisterMode && Index == 1) {
        m_TermsAccepted = !m_TermsAccepted;
        RebuildRows();
    } else if(Index == Base) {
        m_Context->Nav.Pop(); // back to the homeserver picker
    } else if(Index == Base + 1) {
        if(Keyboard::QueryText("Username", "Your user name, without the server", m_UserName)) {
            m_UserName = Util::Trim(m_UserName);
            RebuildRows();
        }
    } else if(Index == Base + 2) {
        std::string Input;
        if(Keyboard::QueryPassword("Password", "Your account password", Input)) {
            m_Password = Input;
            RebuildRows();
        }
    } else if(Index == Base + 3) {
        OnSubmit();
    }
}

void LoginLayout::OnSubmit() {
    if(m_Busy) {
        return;
    }
    m_UserName = Util::Trim(m_UserName);
    if(m_UserName.empty()) {
        SetStatusText(m_Status, "Enter your user name.", Theme::Danger);
        return;
    }
    if(m_IsRegisterMode) {
        if(m_Password.size() < 8) {
            SetStatusText(m_Status, "Password must be at least 8 characters.", Theme::Danger);
            return;
        }
        if(!m_TermsAccepted) {
            SetStatusText(m_Status, "Accept the homeserver terms to continue.", Theme::Danger);
            return;
        }
    } else if(m_Password.empty()) {
        SetStatusText(m_Status, "Enter your password.", Theme::Danger);
        return;
    }

    m_Busy = true;
    SetStatusText(m_Status, m_IsRegisterMode ? "Creating account..." : "Signing in...", Theme::TextSecondary);

    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const auto User = m_UserName;
    const auto Password = m_Password;
    const auto Register = m_IsRegisterMode;
    Core::JobQueue::Instance().Enqueue([this, Ctx, Alive, User, Password, Register]() {
        auto Result = Register ? Ctx->Api.Register(User, Password) : Ctx->Api.LoginPassword(User, Password);
        Core::Dispatcher::Instance().Post([this, Ctx, Alive, Result = std::move(Result)]() mutable {
            if(!Result.Ok) {
                if(*Alive) {
                    m_Busy = false;
                    SetStatusText(m_Status, "Sign-in failed.", Theme::Danger);
                }
                ShowError(Ctx->Nav.GetApp(), "Sign-in failed", Result.Error.ToDisplayString());
                return;
            }
            // Leaving the screen while the request ran cancels the flow.
            if(!*Alive) {
                return;
            }
            OnAuthenticated(Result.Value);
        });
    });
}

void LoginLayout::OnAuthenticated(const Matrix::LoginResult &Result) {
    m_Busy = false;
    SetStatusText(m_Status, "", Theme::TextSecondary);

    m_Context->Account.AccessToken = Result.AccessToken;
    m_Context->Account.RefreshToken = Result.RefreshToken;
    m_Context->Account.DeviceId = Result.DeviceId;
    m_Context->Account.UserId = Result.UserId;
    m_Context->Profile.UserId = Result.UserId;
    m_Context->Store.SavePublicProfile(m_Context->Profile);

    m_Context->Nav.Push(PinLayout::New(m_Context, PinLayout::Mode::Create));
}

} // namespace NxMatrix::Ui