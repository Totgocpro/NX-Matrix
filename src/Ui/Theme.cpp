#include "NxMatrix/Ui/Theme.hpp"

#include <switch.h>

namespace NxMatrix::Ui::Theme {

bool DetectSystemDark() {
    ColorSetId Id = ColorSetId_Dark;
    if(R_SUCCEEDED(setsysInitialize())) {
        if(R_FAILED(setsysGetColorSetId(&Id))) {
            Id = ColorSetId_Dark;
        }
        setsysExit();
    }
    return Id == ColorSetId_Dark;
}

} // namespace NxMatrix::Ui::Theme
