// Themed dialog/toast helpers: one prepare callback gives every dialog the
// same cyan-on-dark palette.
#include "NxMatrix/Ui/Dialogs.hpp"

#include "NxMatrix/Ui/Theme.hpp"

namespace NxMatrix::Ui {

namespace {

void ApplyTheme(pu::ui::Dialog::Ref &DialogRef) {
    DialogRef->SetTitleColor(Theme::Accent);
    DialogRef->SetContentColor(Theme::TextSecondary);
    DialogRef->SetOptionColor(Theme::TextPrimary);
    DialogRef->SetDialogColor(Theme::SurfaceRaised);
    DialogRef->SetOverColor(Theme::AccentDim);
}

void ShowThemed(pu::ui::Application &App, const std::string &Title, const std::string &Message) {
    App.CreateShowDialog(Title, Message, {"OK"}, false, nullptr, ApplyTheme);
}

} // namespace

void ShowMessage(pu::ui::Application &App, const std::string &Title, const std::string &Message) {
    ShowThemed(App, Title, Message);
}

void ShowError(pu::ui::Application &App, const std::string &Title, const std::string &Message) {
    ShowThemed(App, "Error", Title + "\n" + Message);
}

bool Confirm(pu::ui::Application &App, const std::string &Title, const std::string &Message, const std::string &ConfirmLabel,
             const std::string &CancelLabel) {
    const auto Result = App.CreateShowDialog(Title, Message, {ConfirmLabel, CancelLabel}, true, nullptr, ApplyTheme);
    return Result == 0;
}

std::string ShowOptions(pu::ui::Application &App, const std::string &Title, const std::string &Message,
                        const std::vector<std::string> &Options) {
    const auto Result = App.CreateShowDialog(Title, Message, Options, false, nullptr, ApplyTheme);
    if(Result < 0 || static_cast<std::size_t>(Result) >= Options.size()) {
        return "";
    }
    return Options[static_cast<std::size_t>(Result)];
}

void ShowToast(pu::ui::Application &App, const std::string &Message, std::uint64_t DurationMs) {
    auto Text = pu::ui::elm::TextBlock::New(0, 0, Message);
    Text->SetFont(Theme::FontSmall);
    Text->SetColor(Theme::TextPrimary);
    auto Toast = pu::ui::extras::Toast::New(Text, Theme::SurfaceRaised);
    App.StartOverlayWithTimeout(Toast, DurationMs);
}

} // namespace NxMatrix::Ui