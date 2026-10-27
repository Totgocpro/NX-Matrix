#pragma once
#include "NxMatrix/Matrix/MediaLoader.hpp"
#include "NxMatrix/Types.hpp"
#include "NxMatrix/Ui/Theme.hpp"

#include <pu/Plutonium>

#include <memory>
#include <string>

namespace NxMatrix::Ui {

// Circular avatar: asynchronously loaded mxc:// media, or a colored circle
// with the first letter of the display/user name as fallback.
class Avatar : public pu::ui::elm::Element {
public:
    Avatar(std::int32_t X, std::int32_t Y, std::int32_t Diameter);
    ~Avatar();
    // PU_SMART_CTOR already declares `using Ref = std::shared_ptr<Avatar>;` plus New().
    PU_SMART_CTOR(Avatar)

    // Media may be nullptr (initial-letter fallback only). UserId drives the
    // fallback color and letter.
    void SetUser(const std::string &UserId, const std::string &DisplayName, const std::string &MxcUrl, Matrix::MediaLoader *Media);

    std::int32_t GetX() override;
    std::int32_t GetY() override;
    std::int32_t GetWidth() override;
    std::int32_t GetHeight() override;
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;
    void OnInput(const std::uint64_t KeysDown, const std::uint64_t KeysUp, const std::uint64_t KeysHeld,
                 const pu::ui::TouchPoint TouchPos) override;

private:
    void OnImageLoaded(bool Success, const ByteArray &Data);
    void SetTexture(pu::sdl2::TextureHandle::Ref Texture);
    void RebuildLetter();

    std::int32_t m_X;
    std::int32_t m_Y;
    std::int32_t m_Diameter;
    std::string m_UserId;
    std::string m_DisplayName;
    std::string m_RequestedMxc;
    pu::sdl2::TextureHandle::Ref m_Texture;
    bool m_Loading = false;
    std::string m_Letter;
    pu::ui::Color m_BgColor = Theme::Background;
    pu::ui::Color m_LetterColor = Theme::Accent;
    pu::sdl2::Texture m_LetterTex = nullptr;
    // Cleared by the destructor so an in-flight media callback (posted to the
    // UI thread) can detect that this element is gone.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace NxMatrix::Ui