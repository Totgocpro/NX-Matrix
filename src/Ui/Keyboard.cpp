// Horizon software keyboard wrapper. swkbdShow fails with LibAppletBadExit
// when the user cancels, leaving the output buffer untouched - that is the
// only cancel signal, so a failed Result means "keep InOutText as-is".
#include "NxMatrix/Ui/Keyboard.hpp"

#include <switch.h>

#include <functional>
#include <vector>

namespace NxMatrix::Ui {

namespace {

bool RunKeyboard(const std::function<void(SwkbdConfig &)> &Configure, std::size_t MaxLen, std::string &InOutText) {
    SwkbdConfig Kbd;
    if(R_FAILED(swkbdCreate(&Kbd, 0))) {
        return false;
    }
    Configure(Kbd);
    swkbdConfigSetStringLenMax(&Kbd, static_cast<u32>(MaxLen));
    swkbdConfigSetOkButtonText(&Kbd, "OK");

    std::vector<char> Out(MaxLen + 1, '\0');
    const auto Rc = swkbdShow(&Kbd, Out.data(), Out.size());
    swkbdClose(&Kbd);
    if(R_FAILED(Rc)) {
        return false; // cancelled: InOutText stays untouched
    }
    InOutText = std::string(Out.data());
    return true;
}

} // namespace

bool Keyboard::QueryText(const std::string &Title, const std::string &Guide, std::string &InOutText, std::size_t MaxLen) {
    return RunKeyboard(
        [&](SwkbdConfig &Kbd) {
            swkbdConfigMakePresetDefault(&Kbd);
            swkbdConfigSetHeaderText(&Kbd, Title.c_str());
            swkbdConfigSetGuideText(&Kbd, Guide.c_str());
        },
        MaxLen, InOutText);
}

bool Keyboard::QueryLongText(const std::string &Title, const std::string &Guide, std::string &InOutText, std::size_t MaxLen) {
    // Same default preset, larger length cap for topics/messages.
    return RunKeyboard(
        [&](SwkbdConfig &Kbd) {
            swkbdConfigMakePresetDefault(&Kbd);
            swkbdConfigSetHeaderText(&Kbd, Title.c_str());
            swkbdConfigSetGuideText(&Kbd, Guide.c_str());
        },
        MaxLen, InOutText);
}

bool Keyboard::QueryPassword(const std::string &Title, const std::string &Guide, std::string &InOutText, std::size_t MinLen,
                             std::size_t MaxLen) {
    return RunKeyboard(
        [&](SwkbdConfig &Kbd) {
            swkbdConfigMakePresetPassword(&Kbd);
            swkbdConfigSetStringLenMin(&Kbd, static_cast<u32>(MinLen));
            swkbdConfigSetHeaderText(&Kbd, Title.c_str());
            swkbdConfigSetGuideText(&Kbd, Guide.c_str());
        },
        MaxLen, InOutText);
}

bool Keyboard::QueryPin(const std::string &Title, const std::string &Guide, std::string &InOutText) {
    return RunKeyboard(
        [&](SwkbdConfig &Kbd) {
            swkbdConfigMakePresetDefault(&Kbd);
            swkbdConfigSetType(&Kbd, SwkbdType_NumPad);
            swkbdConfigSetStringLenMin(&Kbd, 4);
            swkbdConfigSetHeaderText(&Kbd, Title.c_str());
            swkbdConfigSetGuideText(&Kbd, Guide.c_str());
        },
        8, InOutText);
}

} // namespace NxMatrix::Ui