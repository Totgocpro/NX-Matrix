// Header action tile: tile background + outline + tinted PNG glyph.
#include "NxMatrix/Ui/Elements/IconButton.hpp"

#include "NxMatrix/Ui/Theme.hpp"

namespace NxMatrix::Ui {

namespace {

constexpr std::int32_t GlyphPadding = 8;

} // namespace

IconButton::IconButton(std::int32_t X, std::int32_t Y, std::int32_t Size, const std::string &ImagePath)
    : m_X(X), m_Y(Y), m_Size(Size), m_GlyphSize(Size - 2 * GlyphPadding) {
    // The shipped PNGs are white on transparent: the theme colour is an SDL
    // colour mod (multiplicative, so a black glyph could never be tinted).
    if(auto Texture = pu::ui::render::LoadImage(ImagePath)) {
        SDL_SetTextureBlendMode(Texture, SDL_BLENDMODE_BLEND);
        m_Glyph = pu::sdl2::TextureHandle::New(Texture);
        TintGlyph(Theme::TextPrimary);
    }
}

std::int32_t IconButton::GetX() {
    return m_X;
}

std::int32_t IconButton::GetY() {
    return m_Y;
}

std::int32_t IconButton::GetWidth() {
    return m_Size;
}

std::int32_t IconButton::GetHeight() {
    return m_Size;
}

void IconButton::TintGlyph(const pu::ui::Color &Color) {
    if(m_Glyph != nullptr && m_Glyph->Get() != nullptr) {
        SDL_SetTextureColorMod(m_Glyph->Get(), Color.r, Color.g, Color.b);
    }
}

void IconButton::SetFocused(bool Focused) {
    if(m_Focused == Focused) {
        return;
    }
    m_Focused = Focused;
    TintGlyph(Focused ? Theme::Accent : Theme::TextPrimary);
}

bool IconButton::IsFocused() const {
    return m_Focused;
}

void IconButton::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    Drawer->RenderRoundedRectangleFill(m_Focused ? Theme::AccentDim : Theme::SurfaceRaised, X, Y, m_Size, m_Size, Theme::CornerRadius);
    Drawer->RenderRoundedRectangle(m_Focused ? Theme::Accent : Theme::Border, X, Y, m_Size, m_Size, Theme::CornerRadius);
    if(m_Glyph != nullptr && m_Glyph->Get() != nullptr) {
        const std::int32_t Pad = (m_Size - m_GlyphSize) / 2;
        Drawer->RenderTexture(m_Glyph->Get(), X + Pad, Y + Pad,
                              pu::ui::render::TextureRenderOptions({}, m_GlyphSize, m_GlyphSize, {}, {}, {}));
    }
}

} // namespace NxMatrix::Ui