#pragma once
#include <cstddef>
#include <string>

namespace NxMatrix::Ui {

// Wraps the Horizon OS software keyboard (swkbd) for every text entry, so
// typing on the Switch always uses the system keyboard.
class Keyboard {
public:
    // All return false when the user cancelled; InOutText is left untouched then.
    static bool QueryText(const std::string &Title, const std::string &Guide, std::string &InOutText, std::size_t MaxLen = 512);
    static bool QueryLongText(const std::string &Title, const std::string &Guide, std::string &InOutText, std::size_t MaxLen = 4000);
    static bool QueryPassword(const std::string &Title, const std::string &Guide, std::string &InOutText, std::size_t MinLen = 6,
                              std::size_t MaxLen = 128);
    // Numeric keypad, 4 to 8 digits.
    static bool QueryPin(const std::string &Title, const std::string &Guide, std::string &InOutText);
};

} // namespace NxMatrix::Ui