// Homeserver picker: preset rows plus a typed custom server, resolved and
// version-checked off the UI thread before continuing to login.
#include "NxMatrix/Ui/Layouts/HomeServerLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Matrix/Homeserver.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Keyboard.hpp"
#include "NxMatrix/Ui/Layouts/LoginLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Version.hpp"

namespace NxMatrix::Ui {

HomeServerLayout::HomeServerLayout(std::shared_ptr<AppContext> Context) : m_Context(std::move(Context)) {
    this->SetBackgroundColor(Theme::Background);

    auto Title = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "NX-Matrix");
    Title->SetFont(Theme::FontTitle);
    Title->SetColor(Theme::Accent);
    this->Add(Title);

    auto Version = pu::ui::elm::TextBlock::New(0, 46, std::string("v") + AppVersion);
    Version->SetFont(Theme::FontSmall);
    Version->SetColor(Theme::TextFaint);
    Version->SetX(Theme::ScreenWidth - Theme::ScreenMargin - Version->GetWidth());
    this->Add(Version);

    auto Subtitle = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 96, "Choose a homeserver");
    Subtitle->SetFont(Theme::FontSmall);
    Subtitle->SetColor(Theme::TextSecondary);
    this->Add(Subtitle);

    m_Status = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 96, "");
    m_Status->SetFont(Theme::FontSmall);
    m_Status->SetColor(Theme::Warning);
    this->Add(m_Status);

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A Select   Minus Quit");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    m_List = std::make_shared<ListWidget>(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                                          Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    this->Add(m_List);

    const auto Width = Theme::ScreenWidth - 2 * Theme::ScreenMargin;
    std::vector<Row::Ref> Rows;
    for(const auto &Preset : Matrix::Homeserver::GetPresets()) {
        Rows.push_back(std::make_shared<SimpleRow>(Width, Preset.Name, Preset.ServerName));
    }
    Rows.push_back(std::make_shared<SimpleRow>(Width, "Custom homeserver..."));
    m_List->SetRows(std::move(Rows));

    m_List->SetOnActivate([this](std::size_t Index) {
        const auto &Presets = Matrix::Homeserver::GetPresets();
        if(Index < Presets.size()) {
            OnServerPicked(Presets[Index].ServerName);
            return;
        }
        std::string Input;
        if(Keyboard::QueryText("Homeserver", "e.g. matrix.org", Input)) {
            ResolveServer(Input);
        }
    });
}

void HomeServerLayout::SetStatus(const std::string &Text, const pu::ui::Color &Color) {
    m_Status->SetColor(Color);
    m_Status->SetText(Text);
    // Right-aligned against the screen margin, opposite the subtitle.
    m_Status->SetX(Theme::ScreenWidth - Theme::ScreenMargin - m_Status->GetWidth());
}

void HomeServerLayout::OnServerPicked(const std::string &ServerName) {
    ResolveServer(ServerName);
}

void HomeServerLayout::ResolveServer(const std::string &ServerName) {
    const auto Name = Matrix::Homeserver::NormalizeServerName(ServerName);
    if(Name.empty()) {
        ShowError(m_Context->Nav.GetApp(), "Invalid homeserver", "Type a server name such as matrix.org.");
        return;
    }
    SetStatus("Resolving...", Theme::Warning);

    auto Ctx = m_Context;
    Core::JobQueue::Instance().Enqueue([this, Ctx, Name]() {
        auto Base = Matrix::Homeserver::ResolveBaseUrl(Name);
        if(!Base.Ok) {
            Core::Dispatcher::Instance().Post([this, Ctx, Err = Base.Error.ToDisplayString()]() {
                SetStatus("", Theme::Warning);
                ShowError(Ctx->Nav.GetApp(), "Homeserver unreachable", Err);
            });
            return;
        }
        Ctx->Api.Configure(Base.Value);
        auto Versions = Ctx->Api.GetVersions();
        if(!Versions.Ok) {
            Core::Dispatcher::Instance().Post([this, Ctx, Err = Versions.Error.ToDisplayString()]() {
                SetStatus("", Theme::Warning);
                ShowError(Ctx->Nav.GetApp(), "Homeserver unreachable", Err);
            });
            return;
        }
        if(!Versions.Value.Supports("v1.1")) {
            Core::Dispatcher::Instance().Post([this, Ctx]() {
                SetStatus("", Theme::Warning);
                ShowError(Ctx->Nav.GetApp(), "Unsupported homeserver", "The server does not advertise Matrix v1.1 support.");
            });
            return;
        }
        Core::Dispatcher::Instance().Post([this, Ctx, Url = Base.Value]() {
            SetStatus("", Theme::Warning);
            Ctx->Account.HomeserverUrl = Url;
            Ctx->Nav.Push(LoginLayout::New(Ctx));
        });
    });
}

} // namespace NxMatrix::Ui