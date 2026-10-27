// Avatar rendering: hashed muted background + first-letter glyph until the
// mxc:// thumbnail arrives; textures are created once (never per frame).
#include "NxMatrix/Ui/Elements/Avatar.hpp"

#include "NxMatrix/Ui/Elements/ImageTexture.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"

namespace NxMatrix::Ui {

namespace {

constexpr std::int32_t ImageInset = 4;

// FNV-1a: stable per user id, good enough to spread ids over the palette.
std::uint64_t HashId(const std::string &Id) {
    std::uint64_t Hash = 14695981039346656037ULL;
    for(const auto Ch : Id) {
        Hash ^= static_cast<unsigned char>(Ch);
        Hash *= 1099511628211ULL;
    }
    return Hash;
}

const pu::ui::Color MutedBackgrounds[] = {
    {0x1B, 0x2E, 0x38, 0xFF},
    {0x23, 0x31, 0x27, 0xFF},
    {0x2B, 0x2D, 0x21, 0xFF},
    {0x2D, 0x25, 0x2B, 0xFF},
};

const pu::ui::Color LetterColors[] = {Theme::Accent, Theme::Success, Theme::Warning, Theme::Danger};

// First UTF-8 glyph of the display name, else of the local part of the user id.
std::string FirstGlyph(const std::string &DisplayName, const std::string &UserId) {
    auto Source = Util::Trim(DisplayName);
    if(Source.empty()) {
        Source = UserId;
        if(!Source.empty() && Source.front() == '@') {
            Source.erase(0, 1);
        }
        const auto Colon = Source.find(':');
        if(Colon != std::string::npos) {
            Source = Source.substr(0, Colon);
        }
    }
    if(Source.empty()) {
        return "?";
    }
    std::size_t End = 1;
    while(End < Source.size() && (static_cast<unsigned char>(Source[End]) & 0xC0) == 0x80) {
        End++;
    }
    return Source.substr(0, End);
}

} // namespace

Avatar::Avatar(std::int32_t X, std::int32_t Y, std::int32_t Diameter) : m_X(X), m_Y(Y), m_Diameter(Diameter) {}

Avatar::~Avatar() {
    *m_Alive = false;
    pu::ui::render::DeleteTexture(m_LetterTex);
}

void Avatar::RebuildLetter() {
    pu::ui::render::DeleteTexture(m_LetterTex);
    m_Letter = FirstGlyph(m_DisplayName, m_UserId);
    const auto Hash = HashId(m_UserId);
    m_BgColor = MutedBackgrounds[Hash % (sizeof(MutedBackgrounds) / sizeof(MutedBackgrounds[0]))];
    m_LetterColor = LetterColors[(Hash >> 8) % (sizeof(LetterColors) / sizeof(LetterColors[0]))];
    m_LetterTex = pu::ui::render::RenderText(Theme::FontHeading, m_Letter, m_LetterColor);
}

void Avatar::SetUser(const std::string &UserId, const std::string &DisplayName, const std::string &MxcUrl, Matrix::MediaLoader *Media) {
    const bool UserChanged = (UserId != m_UserId) || (DisplayName != m_DisplayName);
    m_UserId = UserId;
    m_DisplayName = DisplayName;
    if(UserChanged || (m_LetterTex == nullptr && MxcUrl.empty())) {
        RebuildLetter();
    }
    if(MxcUrl.empty() || Media == nullptr) {
        m_RequestedMxc.clear();
        m_Loading = false;
        m_Texture = nullptr;
        return;
    }
    if(MxcUrl == m_RequestedMxc) {
        // Already showing it or waiting for it.
        if(m_Texture != nullptr || m_Loading) {
            return;
        }
    } else {
        // Different image requested: drop the stale one immediately.
        m_Texture = nullptr;
    }
    m_RequestedMxc = MxcUrl;
    m_Loading = true;
    Media->LoadMxc(MxcUrl, 96, 96, [Flag = m_Alive, Requested = MxcUrl, this](bool Success, const ByteArray &Data) {
        // MediaLoader delivers on the UI thread; skip when we were replaced
        // or destroyed while the request was in flight.
        if(!*Flag || Requested != m_RequestedMxc) {
            return;
        }
        m_Loading = false;
        OnImageLoaded(Success, Data);
    });
}

void Avatar::OnImageLoaded(bool Success, const ByteArray &Data) {
    if(!Success) {
        return; // keep the letter fallback
    }
    SetTexture(DecodeImageTexture(Data)); // nullptr decode also keeps the letter
}

void Avatar::SetTexture(pu::sdl2::TextureHandle::Ref Texture) {
    m_Texture = std::move(Texture);
}

std::int32_t Avatar::GetX() {
    return m_X;
}

std::int32_t Avatar::GetY() {
    return m_Y;
}

std::int32_t Avatar::GetWidth() {
    return m_Diameter;
}

std::int32_t Avatar::GetHeight() {
    return m_Diameter;
}

void Avatar::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    Drawer->RenderRoundedRectangleFill(m_BgColor, X, Y, m_Diameter, m_Diameter, Theme::CornerRadius);
    if(m_Texture != nullptr) {
        Drawer->RenderTexture(
            m_Texture->Get(), X + ImageInset, Y + ImageInset,
            pu::ui::render::TextureRenderOptions({}, m_Diameter - 2 * ImageInset, m_Diameter - 2 * ImageInset, {}, {}, {}));
    } else if(m_LetterTex != nullptr) {
        const auto LetterW = pu::ui::render::GetTextureWidth(m_LetterTex);
        const auto LetterH = pu::ui::render::GetTextureHeight(m_LetterTex);
        Drawer->RenderTexture(m_LetterTex, X + (m_Diameter - LetterW) / 2, Y + (m_Diameter - LetterH) / 2);
    }
    // Thin ring over the content so the card edge stays visible.
    Drawer->RenderRoundedRectangle(Theme::Border, X, Y, m_Diameter, m_Diameter, Theme::CornerRadius);
}

void Avatar::OnInput(const std::uint64_t, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {}

} // namespace NxMatrix::Ui