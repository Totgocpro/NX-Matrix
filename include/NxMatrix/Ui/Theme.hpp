#pragma once
#include <pu/Plutonium>

#include <cstdint>
#include <string>

namespace NxMatrix::Ui {

// Cyan-on-dark theme: every screen takes its colors, fonts and metrics from
// here so the look stays consistent and easy to retune in one place.
namespace Theme {

// --- Colors ---
constexpr pu::ui::Color Background{0x0A, 0x11, 0x16, 0xFF};
constexpr pu::ui::Color Surface{0x10, 0x1B, 0x22, 0xFF};
constexpr pu::ui::Color SurfaceRaised{0x16, 0x25, 0x2E, 0xFF};
constexpr pu::ui::Color Accent{0x00, 0xE5, 0xFF, 0xFF};
constexpr pu::ui::Color AccentDim{0x0B, 0x8B, 0xA0, 0xFF};
constexpr pu::ui::Color TextPrimary{0xE8, 0xF6, 0xFA, 0xFF};
constexpr pu::ui::Color TextSecondary{0x93, 0xB4, 0xBE, 0xFF};
constexpr pu::ui::Color TextFaint{0x5B, 0x7A, 0x85, 0xFF};
constexpr pu::ui::Color Success{0x3E, 0xDC, 0x97, 0xFF};
constexpr pu::ui::Color Warning{0xFF, 0xC2, 0x4D, 0xFF};
constexpr pu::ui::Color Danger{0xFF, 0x5D, 0x6C, 0xFF};
constexpr pu::ui::Color Border{0x1D, 0x33, 0x3D, 0xFF};
constexpr pu::ui::Color BubbleOwn{0x0D, 0x4B, 0x5B, 0xFF};
constexpr pu::ui::Color BubbleOther{0x15, 0x28, 0x32, 0xFF};

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