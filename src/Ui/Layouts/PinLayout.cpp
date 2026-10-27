// PIN/password creation (first login) and change: validates the two entries,
// seals the vault and either starts the session or returns to settings.
#include "NxMatrix/Ui/Layouts/PinLayout.hpp"

#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/RoomListLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"

#include <algorithm>
#include <cctype>

namespace NxMatrix::Ui {

namespace {

void SetStatusText(pu::ui::elm::TextBlock::Ref &Status, const std::string &Text, const pu::ui::Color &Color) {
    Status->SetColor(Color);
    Status->SetText(Text);
}

bool IsAllDigits(const std::string &Text) {
    return std::all_of(Text.begin(), Text.end(), [](char Ch) { return std::isdigit(static_cast<unsigned char>(Ch)) != 0; });
}

} // namespace

PinLayout::PinLayout(std::shared_ptr<AppContext> Context, Mode LayoutMode) : m_Context(std::move(Context)), m_Mode(LayoutMode) {
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

void PinLayout::RebuildRows() {
    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    std::vector<Row::Ref> Rows;
    if(m_Mode == Mode::Create) {
        Rows.push_back(std::make_shared<ToggleRow>(Width, "Use a password instead of a PIN", m_UsePassword));
    }
    Rows.push_back(
        std::make_shared<TextInputRow>(Width, m_UsePassword ? "Password" : "PIN", std::string(m_Secret.size(), '*'), "Tap A to enter"));
    Rows.push_back(std::make_shared<TextInputRow>(Width, "Confirm", std::string(m_Confirm.size(), '*'), "Tap A to enter"));
    Rows.push_back(std::make_shared<SimpleRow>(Width, "Confirm"));
    m_List->SetRows(std::move(Rows));
}

void PinLayout::OnFieldActivated(std::size_t Index) {
    const std::size_t Base = (m_Mode == Mode::Create) ? 1 : 0;
    if(m_Mode == Mode::Create && Index == 0) {
        m_UsePassword = !m_UsePassword;
        m_Secret.clear();
        m_Confirm.clear();
        RebuildRows();
        return;
    }
    if(Index == Base) {
        std::string Input;
        const bool Ok = m_UsePassword ? Keyboard::QueryPassword("Password", "At least 8 characters", Input, 8, 128)
                                      : Keyboard::QueryPin("PIN", "4 to 8 digits", Input);
        if(Ok) {
            m_Secret = Input;
            RebuildRows();
        }
    } else if(Index == Base + 1) {
        std::string Input;
        const bool Ok = m_UsePassword ? Keyboard::QueryPassword("Confirm password", "Type the password again", Input, 8, 128)
                                      : Keyboard::QueryPin("Confirm PIN", "Type the PIN again", Input);
        if(Ok) {
            m_Confirm = Input;
            RebuildRows();
        }
    } else if(Index == Base + 2) {
        OnConfirm();
    }
}

void PinLayout::OnConfirm() {
    if(m_UsePassword) {
        if(m_Secret.size() < 8) {
            SetStatusText(m_Status, "Password must be at least 8 characters.", Theme::Danger);
            return;
        }
    } else if(m_Secret.size() < 4 || m_Secret.size() > 8 || !IsAllDigits(m_Secret)) {
        SetStatusText(m_Status, "PIN must be 4 to 8 digits.", Theme::Danger);
        return;
    }
    if(m_Secret != m_Confirm) {
        SetStatusText(m_Status, "Both entries must match.", Theme::Danger);
        return;
    }
    Finish(m_Secret);
}

void PinLayout::Finish(const std::string &Secret) {
    if(m_Mode == Mode::Create) {
        Store::Vault::Create(Secret, m_Context->Vault);
        m_Context->PersistAccount();
        m_Context->Store.SavePublicProfile(m_Context->Profile);
        m_Context->StartSession();
        m_Context->Nav.ReplaceRoot(RoomListLayout::New(m_Context));
    } else {
        Store::Vault::Create(Secret, m_Context->Vault);
        // Cached media was sealed with the old key; drop it instead of
        // leaving unreadable blobs behind.
        m_Context->Store.ClearMediaCache();
        m_Context->PersistAccount();
        m_Context->Nav.Pop();
    }
}

} // namespace NxMatrix::Ui