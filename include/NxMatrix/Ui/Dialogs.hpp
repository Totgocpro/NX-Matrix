#pragma once
#include <cstdint>
#include <pu/Plutonium>

#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Themed dialog/toast helpers (Plutonium Dialog and Toast under the hood).
void ShowMessage(pu::ui::Application &App, const std::string &Title, const std::string &Message);
void ShowError(pu::ui::Application &App, const std::string &Title, const std::string &Message);

// Returns true when the user picked the confirm option.
bool Confirm(pu::ui::Application &App, const std::string &Title, const std::string &Message, const std::string &ConfirmLabel = "OK",
             const std::string &CancelLabel = "Cancel");

// Returns the chosen option label, or "" when cancelled.
std::string ShowOptions(pu::ui::Application &App, const std::string &Title, const std::string &Message,
                        const std::vector<std::string> &Options);

void ShowToast(pu::ui::Application &App, const std::string &Message, std::uint64_t DurationMs = 1800);

} // namespace NxMatrix::Ui