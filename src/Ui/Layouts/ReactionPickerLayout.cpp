#include "NxMatrix/Ui/Layouts/ReactionPickerLayout.hpp"

#include "NxMatrix/Ui/Elements/RichText.hpp"
#include "NxMatrix/Ui/Theme.hpp"

namespace NxMatrix::Ui {
namespace {

constexpr std::int32_t ChoiceHeight = 96;
constexpr std::int32_t PadX = 24;

// One selectable choice: a big emoji glyph (or plain text fallback) rasterized
// once through RichText, cached as a texture like every other row.
class EmojiChoiceRow : public Row {
public:
    EmojiChoiceRow(std::int32_t Width, const std::string &Choice) : Row(Width, ChoiceHeight) {
        m_Glyph = RenderRichText(Theme::FontHeading, Choice, Theme::TextPrimary);
    }

    ~EmojiChoiceRow() override {
        if(m_Glyph != nullptr) {
            pu::ui::render::DeleteTexture(m_Glyph);
        }
    }

    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override {
        if(IsFocused()) {
            Drawer->RenderRoundedRectangleFill(Theme::SurfaceRaised, X, Y, m_Width, m_Height, Theme::CornerRadius);
            Drawer->RenderRectangleFill(Theme::Accent, X, Y, 4, m_Height);
        }
        if(m_Glyph == nullptr) {
            return;
        }
        const std::int32_t GlyphH = pu::ui::render::GetTextureHeight(m_Glyph);
        Drawer->RenderTexture(m_Glyph, X + PadX, Y + (m_Height - GlyphH) / 2);
    }

private:
    pu::sdl2::Texture m_Glyph = nullptr;
};

} // namespace

ReactionPickerLayout::ReactionPickerLayout(std::shared_ptr<AppContext> Context, std::vector<std::string> Choices, PickedCallback OnPicked)
    : m_Context(std::move(Context)), m_Choices(std::move(Choices)), m_OnPicked(std::move(OnPicked)) {
    this->SetBackgroundColor(Theme::Background);

    auto Title = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 36, "React with");
    Title->SetFont(Theme::FontHeading);
    Title->SetColor(Theme::Accent);
    this->Add(Title);

    auto Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 58, "A Select    B Back");
    Hint->SetFont(Theme::FontTiny);
    Hint->SetColor(Theme::TextFaint);
    this->Add(Hint);

    m_List = ListWidget::New(Theme::ScreenMargin, Theme::ListTop, Theme::ScreenWidth - 2 * Theme::ScreenMargin,
                             Theme::ScreenHeight - Theme::ListTop - Theme::ListBottom);
    std::vector<Row::Ref> Rows;
    Rows.reserve(m_Choices.size());
    for(const auto &Choice : m_Choices) {
        Rows.push_back(std::make_shared<EmojiChoiceRow>(Theme::ScreenWidth - 2 * Theme::ScreenMargin, Choice));
    }
    m_List->SetRows(std::move(Rows));
    m_List->SetOnActivate([this](std::size_t Index) {
        if(Index >= m_Choices.size() || !m_OnPicked) {
            return;
        }
        // Copy before navigating away: the Pop below can destroy this layout.
        const auto Callback = m_OnPicked;
        const std::string Choice = m_Choices[Index];
        auto Ctx = m_Context;
        Callback(Choice);
        Ctx->Nav.Pop();
    });
    this->Add(m_List);
}

} // namespace NxMatrix::Ui