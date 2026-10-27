#pragma once
#include <pu/Plutonium>

#include <cstdint>
#include <string>

namespace NxMatrix::Ui {

// Header action tile: rounded button carrying a white PNG glyph (packed in the
// romfs) tinted to the theme colour. Input is routed by the owning layout (the
// D-pad walks the tiles), so OnInput stays empty like the other static elements.
class IconButton : public pu::ui::elm::Element {
public:
    IconButton(std::int32_t X, std::int32_t Y, std::int32_t Size, const std::string &ImagePath);
    PU_SMART_CTOR(IconButton)

    std::int32_t GetX() override;
    std::int32_t GetY() override;
    std::int32_t GetWidth() override;
    std::int32_t GetHeight() override;
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;
    void OnInput(const std::uint64_t KeysDown, const std::uint64_t KeysUp, const std::uint64_t KeysHeld,
                 const pu::ui::TouchPoint TouchPos) override {}

    void SetFocused(bool Focused);
    bool IsFocused() const;

private:
    void TintGlyph(const pu::ui::Color &Color);

    std::int32_t m_X;
    std::int32_t m_Y;
    std::int32_t m_Size;
    std::int32_t m_GlyphSize;
    bool m_Focused = false;
    pu::sdl2::TextureHandle::Ref m_Glyph;
};

} // namespace NxMatrix::Ui