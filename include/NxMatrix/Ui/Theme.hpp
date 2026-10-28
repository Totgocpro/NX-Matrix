#pragma once
#include <pu/Plutonium>

#include <cstdint>
#include <string>

namespace NxMatrix::Ui {

// Every screen takes its colors, fonts and metrics from here so the look stays
// consistent and easy to retune in one place. Colors are runtime variables so
// the settings screen can switch between the dark and light palettes.
namespace Theme {

struct Palette {
    pu::ui::Color Background;
    pu::ui::Color Surface;
    pu::ui::Color SurfaceRaised;
    pu::ui::Color Accent;
    pu::ui::Color AccentDim;
    pu::ui::Color TextPrimary;
    pu::ui::Color TextSecondary;
    pu::ui::Color TextFaint;
    pu::ui::Color Success;
    pu::ui::Color Warning;
    pu::ui::Color Danger;
    pu::ui::Color Border;
    pu::ui::Color BubbleOwn;
    pu::ui::Color BubbleOther;
};

// Cyan-on-dark (default) and cyan-on-light palettes.
inline constexpr Palette DarkPalette{
    {0x0A, 0x11, 0x16, 0xFF}, {0x10, 0x1B, 0x22, 0xFF}, {0x16, 0x25, 0x2E, 0xFF}, {0x00, 0xE5, 0xFF, 0xFF}, {0x0B, 0x8B, 0xA0, 0xFF},
    {0xE8, 0xF6, 0xFA, 0xFF}, {0x93, 0xB4, 0xBE, 0xFF}, {0x5B, 0x7A, 0x85, 0xFF}, {0x3E, 0xDC, 0x97, 0xFF}, {0xFF, 0xC2, 0x4D, 0xFF},
    {0xFF, 0x5D, 0x6C, 0xFF}, {0x1D, 0x33, 0x3D, 0xFF}, {0x0D, 0x4B, 0x5B, 0xFF}, {0x15, 0x28, 0x32, 0xFF},
};

inline constexpr Palette LightPalette{
    {0xF2, 0xF7, 0xF9, 0xFF}, {0xE6, 0xEF, 0xF2, 0xFF}, {0xDC, 0xE8, 0xEC, 0xFF}, {0x00, 0x7A, 0x8C, 0xFF}, {0xA9, 0xD8, 0xE2, 0xFF},
    {0x0E, 0x22, 0x29, 0xFF}, {0x3F, 0x5C, 0x66, 0xFF}, {0x6D, 0x87, 0x90, 0xFF}, {0x0E, 0x8A, 0x5F, 0xFF}, {0xB2, 0x6A, 0x00, 0xFF},
    {0xC6, 0x2F, 0x3E, 0xFF}, {0xC3, 0xD4, 0xDA, 0xFF}, {0xBF, 0xE8, 0xEF, 0xFF}, {0xE1, 0xEA, 0xEE, 0xFF},
};

// --- Colors (current palette) ---
inline pu::ui::Color Background = DarkPalette.Background;
inline pu::ui::Color Surface = DarkPalette.Surface;
inline pu::ui::Color SurfaceRaised = DarkPalette.SurfaceRaised;
inline pu::ui::Color Accent = DarkPalette.Accent;
inline pu::ui::Color AccentDim = DarkPalette.AccentDim;
inline pu::ui::Color TextPrimary = DarkPalette.TextPrimary;
inline pu::ui::Color TextSecondary = DarkPalette.TextSecondary;
inline pu::ui::Color TextFaint = DarkPalette.TextFaint;
inline pu::ui::Color Success = DarkPalette.Success;
inline pu::ui::Color Warning = DarkPalette.Warning;
inline pu::ui::Color Danger = DarkPalette.Danger;
inline pu::ui::Color Border = DarkPalette.Border;
inline pu::ui::Color BubbleOwn = DarkPalette.BubbleOwn;
inline pu::ui::Color BubbleOther = DarkPalette.BubbleOther;

inline bool DarkMode = true;

inline bool IsDark() {
    return DarkMode;
}

// Switches the palette used by every screen created afterwards.
inline void SetDark(const bool Dark) {
    const auto &P = Dark ? DarkPalette : LightPalette;
    Background = P.Background;
    Surface = P.Surface;
    SurfaceRaised = P.SurfaceRaised;
    Accent = P.Accent;
    AccentDim = P.AccentDim;
    TextPrimary = P.TextPrimary;
    TextSecondary = P.TextSecondary;
    TextFaint = P.TextFaint;
    Success = P.Success;
    Warning = P.Warning;
    Danger = P.Danger;
    Border = P.Border;
    BubbleOwn = P.BubbleOwn;
    BubbleOther = P.BubbleOther;
    DarkMode = Dark;
}

// Reads the console's current theme (set:sys). Defaults to dark when the
// service is unavailable (e.g. some emulators).
bool DetectSystemDark();

// --- Fonts (extra sizes 22/24 are registered in main.cpp) ---
inline const std::string FontTitle = pu::ui::MakeDefaultFontName(45);
inline const std::string FontHeading = pu::ui::MakeDefaultFontName(37);
inline const std::string FontBody = pu::ui::MakeDefaultFontName(30);
inline const std::string FontSmall = pu::ui::MakeDefaultFontName(27);
inline const std::string FontTiny = pu::ui::MakeDefaultFontName(22);

// --- Metrics ---
constexpr std::int32_t ScreenWidth = pu::ui::render::ScreenWidth;
constexpr std::int32_t ScreenHeight = pu::ui::render::ScreenHeight;
constexpr std::int32_t ScreenMargin = 60;
constexpr std::int32_t HeaderHeight = 96;
constexpr std::int32_t RowHeight = 96;
constexpr std::int32_t RowHeightCompact = 72;
constexpr std::int32_t CornerRadius = 12;
constexpr std::int32_t ListTop = 150;
constexpr std::int32_t ListBottom = 96;

} // namespace Theme
} // namespace NxMatrix::Ui
