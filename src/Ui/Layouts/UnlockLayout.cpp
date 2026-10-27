// Boot screen: restore the vault with the stored KDF parameters, then hand
// over to the session; escape hatches switch account or wipe the SD card.
#include "NxMatrix/Ui/Layouts/UnlockLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/HomeServerLayout.hpp"
#include "NxMatrix/Ui/Layouts/RoomListLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"

#include <string>

namespace NxMatrix::Ui {

namespace {

void SetStatusText(pu::ui::elm::TextBlock::Ref &Status, const std::string &Text, const pu::ui::Color &Color) {
    Status->SetColor(Color);
    Status->SetText(Text);
}

} // namespace

UnlockLayout::UnlockLayout(std::shared_ptr<AppContext> Context) : m_Context(std::move(Context)) {
    this->SetBackgroundColor(Theme::Background);

    auto Title = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "NX-Matrix");
    Title->SetFont(Theme::FontTitle);
    Title->SetColor(Theme::Accent);
    this->Add(Title);

    Store::PublicProfile Stored;
    if(m_Context->Store.LoadPublicProfile(Stored) && !Stored.UserId.empty()) {
        auto Greeting = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 96, "Welcome back, " + Stored.UserId);
        Greeting->SetFont(Theme::FontSmall);
        Greeting->SetColor(Theme::TextSecondary);
        this->Add(Greeting);
    }

    m_Status = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 88, "");
    m_Status->SetFont(Theme::FontSmall);
    m_Status->SetColor(Theme::TextSecondary);
    this->Add(m_Status);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);

    m_List->SetOnActivate([this](std::size_t Index) {
        if(Index == 0) {
            if(Keyboard::QueryPin("Enter PIN", "4 to 8 digits", m_Secret)) {
                OnUnlock();
            }
        } else if(Index == 1) {
            if(Keyboard::QueryPassword("Password", "Your local password", m_Secret, 8, 128)) {
                OnUnlock();
            }
        } else if(Index == 3) {
            OnOtherAccount();
        } else if(Index == 4) {
            OnWipe();
        }
    });

    RebuildRows();
}

UnlockLayout::~UnlockLayout() {
    *m_Alive = false;
}

void UnlockLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    std::vector<Row::Ref> Rows;
    Rows.push_back(std::make_shared<SimpleRow>(Width, "Enter PIN"));
    Rows.push_back(std::make_shared<SimpleRow>(Width, "Use password instead"));
    Rows.push_back(std::make_shared<SectionHeaderRow>(Width, ""));
    Rows.push_back(std::make_shared<SimpleRow>(Width, "Log in with another account"));
    auto Erase = std::make_shared<SimpleRow>(Width, "Erase all data");
    Erase->SetLabelColor(Theme::Danger);
    Rows.push_back(Erase);
    m_List->SetRows(std::move(Rows));
}

void UnlockLayout::OnUnlock() {
    if(m_Secret.empty()) {
        SetStatusText(m_Status, "Enter your PIN or password.", Theme::Danger);
        return;
    }
    SetStatusText(m_Status, "Unlocking...", Theme::TextSecondary);

    auto Ctx = m_Context;
    auto Alive = m_Alive;
    const auto Secret = m_Secret;
    Core::JobQueue::Instance().Enqueue([this, Ctx, Alive, Secret]() {
        Crypto::SaltBytes Salt;
        std::uint32_t Iterations = 0;
        if(!Ctx->Store.ReadAccountKdf(Salt, Iterations)) {
            Core::Dispatcher::Instance().Post([this, Ctx, Alive]() {
                if(!*Alive) {
                    return;
                }
                SetStatusText(m_Status, "Account data is damaged.", Theme::Danger);
                ShowError(Ctx->Nav.GetApp(), "Unlock failed", "The stored account cannot be read. Use \"Erase all data\" to start fresh.");
            });
            return;
        }
        if(!Store::Vault::Restore(Secret, Salt, Iterations, Ctx->Vault)) {
            Core::Dispatcher::Instance().Post([this, Alive]() {
                if(!*Alive) {
                    return;
                }
                m_Secret.clear();
                m_Attempts++;
                std::string Message = "Wrong code";
                if(m_Attempts >= 3) {
                    Message += " (" + std::to_string(m_Attempts) + " attempts)";
                }
                SetStatusText(m_Status, Message, Theme::Danger);
            });
            return;
        }
        if(!Ctx->Store.LoadAccount(Ctx->Vault, Ctx->Account)) {
            Core::Dispatcher::Instance().Post([this, Ctx, Alive]() {
                if(!*Alive) {
                    return;
                }
                SetStatusText(m_Status, "Account data is damaged.", Theme::Danger);
                ShowError(Ctx->Nav.GetApp(), "Unlock failed", "The stored account cannot be read. Use \"Erase all data\" to start fresh.");
            });
            return;
        }
        Core::Dispatcher::Instance().Post([this, Alive]() {
            if(*Alive) {
                StartSession();
            }
        });
    });
}

void UnlockLayout::StartSession() {
    m_Context->StartSession();
    m_Context->Nav.ReplaceRoot(RoomListLayout::New(m_Context));
}

void UnlockLayout::OnOtherAccount() {
    auto &App = m_Context->Nav.GetApp();
    if(Confirm(App, "Log in with another account", "Pick a new homeserver and sign in with another account.")) {
        m_Context->Nav.ReplaceRoot(HomeServerLayout::New(m_Context));
    }
}

void UnlockLayout::OnWipe() {
    auto &App = m_Context->Nav.GetApp();
    if(!Confirm(App, "Erase all data", "This deletes your account, keys and cache from the SD card.", "Erase")) {
        return;
    }
    auto Ctx = m_Context;
    Core::JobQueue::Instance().Enqueue([Ctx]() {
        Ctx->Store.Wipe();
        Core::Dispatcher::Instance().Post([Ctx]() {
            Ctx->Api.ClearSession();
            Ctx->Vault = Store::Vault{};
            Ctx->Nav.ReplaceRoot(HomeServerLayout::New(Ctx));
        });
    });
}

} // namespace NxMatrix::Ui