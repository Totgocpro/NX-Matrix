#pragma once
#include <pu/Plutonium>

#include <cstdint>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// True once the bundled color emoji font (romfs) is loaded; rendering then
// falls back to plain TTF output for text-only strings.
bool HasEmojiFont();

// Renders Text mixing emoji glyphs from the bundled color font with the
// Plutonium font at FontName. Returns a raw texture the caller frees with
// pu::ui::render::DeleteTexture (nullptr when nothing rendered).
pu::sdl2::Texture RenderRichText(const std::string &FontName, const std::string &Text, const pu::ui::Color &Color,
                                 std::int32_t MaxWidth = 0);

// Word wrap that measures emoji runs at their rendered width (UTF-8 safe,
// honors \n). Falls back to plain wrapping when the emoji font is missing.
std::vector<std::string> WrapRichText(const std::string &FontName, const std::string &Text, std::int32_t MaxWidth);

} // namespace NxMatrix::Ui