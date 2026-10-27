#pragma once
#include "NxMatrix/Matrix/Models.hpp"
#include "NxMatrix/Ui/Elements/Avatar.hpp"
#include "NxMatrix/Ui/Theme.hpp"

#include <pu/Plutonium>

#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Base for rows hosted by ListWidget: full-width, fixed-height, and aware of
// their focus state so each row draws its own highlight.
class Row : public pu::ui::elm::Element {
public:
    Row(std::int32_t Width, std::int32_t Height);
    using Ref = std::shared_ptr<Row>;

    void SetFocused(bool Focused);
    bool IsFocused() const;
    bool IsSelectable() const;

    std::int32_t GetX() override;
    std::int32_t GetY() override;
    std::int32_t GetWidth() override;
    std::int32_t GetHeight() override;
    void OnInput(const std::uint64_t KeysDown, const std::uint64_t KeysUp, const std::uint64_t KeysHeld,
                 const pu::ui::TouchPoint TouchPos) override;

protected:
    void SetSelectable(bool Selectable);

    std::int32_t m_Width;
    std::int32_t m_Height;
    bool m_Focused = false;
    bool m_Selectable = true;
};

// Non-selectable section title (e.g. "Favourites", "Invites").
class SectionHeaderRow : public Row {
public:
    SectionHeaderRow(std::int32_t Width, const std::string &Title);
    ~SectionHeaderRow();
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    std::string m_Title;
    pu::sdl2::Texture m_TitleTex = nullptr;
};

// Label + right-aligned value + optional hint line under the label.
class SimpleRow : public Row {
public:
    SimpleRow(std::int32_t Width, const std::string &Label, const std::string &Value = {}, const std::string &Hint = {});
    ~SimpleRow();
    void SetValue(const std::string &Value);
    void SetLabelColor(const pu::ui::Color &Color);
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    std::string m_Label;
    std::string m_Value;
    std::string m_Hint;
    pu::ui::Color m_LabelColor = Theme::TextPrimary;
    pu::sdl2::Texture m_LabelTex = nullptr;
    pu::sdl2::Texture m_ValueTex = nullptr;
    pu::sdl2::Texture m_HintTex = nullptr;
    void RenderValue();
};

// Editable field row: shows label + value, A opens the keyboard (the owner
// connects the activation).
class TextInputRow : public Row {
public:
    TextInputRow(std::int32_t Width, const std::string &Label, const std::string &Value, const std::string &Placeholder = {});
    ~TextInputRow();
    std::string GetValue() const;
    void SetValue(const std::string &Value);
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    std::string m_Label;
    std::string m_Value;
    std::string m_Placeholder;
    pu::sdl2::Texture m_LabelTex = nullptr;
    pu::sdl2::Texture m_ValueTex = nullptr;
    void RenderValue();
};

// Label + ON/OFF marker.
class ToggleRow : public Row {
public:
    ToggleRow(std::int32_t Width, const std::string &Label, bool Checked);
    ~ToggleRow();
    bool IsChecked() const;
    void SetChecked(bool Checked);
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    std::string m_Label;
    bool m_Checked = false;
    pu::sdl2::Texture m_LabelTex = nullptr;
    pu::sdl2::Texture m_OnTex = nullptr;
    pu::sdl2::Texture m_OffTex = nullptr;
};

// Room list entry: avatar, name, last message preview, time, unread badge and
// lock/invite chips.
class RoomRow : public Row {
public:
    RoomRow(std::int32_t Width, const Matrix::RoomSummary &Room, Matrix::MediaLoader *Media);
    ~RoomRow();
    void Update(const Matrix::RoomSummary &Room);
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    Matrix::RoomSummary m_Room;
    Avatar::Ref m_Avatar;
    pu::sdl2::Texture m_NameTex = nullptr;
    pu::sdl2::Texture m_PreviewTex = nullptr;
    pu::sdl2::Texture m_TimeTex = nullptr;
    pu::sdl2::Texture m_UnreadTex = nullptr;
    pu::sdl2::Texture m_E2eeTex = nullptr;
    pu::sdl2::Texture m_InviteTex = nullptr;
    void RebuildTextures();
};

// Timeline entry: sender header, wrapped body, reply quote block, reactions,
// time and state/membership styling. Height adapts to the wrapped body.
class MessageRow : public Row {
public:
    MessageRow(std::int32_t Width, const Matrix::Event &Event, const std::string &SenderName, bool ShowSender, bool OwnMessage,
               const std::vector<Matrix::ReactionGroup> &Reactions, const std::string &ReplySender, const std::string &ReplyPreview,
               Matrix::MediaLoader *Media);
    ~MessageRow();
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    // Pre-rendered reaction chip (text measured once, offsets relative to the
    // reactions block origin inside the bubble).
    struct ReactionChipTex {
        pu::sdl2::Texture Tex = nullptr;
        std::int32_t W = 0;
        std::int32_t H = 0;
        std::int32_t X = 0;
        std::int32_t Y = 0;
        bool Mine = false;
    };

    // Inline image preview: the box is reserved at construction (row height is
    // fixed) and filled asynchronously by MediaLoader.
    enum class ImageState { Pending, Ready, Failed };

    Matrix::Event m_Event;
    std::string m_SenderName;
    std::string m_Body;
    std::string m_ReplySender;
    std::string m_ReplyPreview;
    std::string m_TimeText;
    std::vector<std::string> m_WrappedLines;
    std::vector<Matrix::ReactionGroup> m_Reactions;
    Avatar::Ref m_Avatar;
    bool m_ShowSender = false;
    bool m_OwnMessage = false;
    pu::ui::Color m_BodyColor = Theme::TextPrimary;
    pu::sdl2::Texture m_SenderTex = nullptr;
    pu::sdl2::Texture m_TimeTex = nullptr;
    pu::sdl2::Texture m_ReplyHeadTex = nullptr;
    pu::sdl2::Texture m_ReplyTex = nullptr;
    pu::sdl2::Texture m_AttachTex = nullptr;
    std::vector<pu::sdl2::Texture> m_LineTexs;
    std::vector<ReactionChipTex> m_ReactionChips;
    // Geometry relative to the row: bubble box plus per-section Y offsets
    // (-1 = section absent), all resolved once in the constructor.
    std::int32_t m_BubbleRelX = 0;
    std::int32_t m_BubbleW = 0;
    std::int32_t m_BubbleH = 0;
    std::int32_t m_SenderRelY = -1;
    std::int32_t m_ReplyRelY = -1; // quote block top (-1 = not a reply)
    std::int32_t m_ReplyBlockW = 0;
    std::int32_t m_ReplyBlockH = 0;
    std::int32_t m_AttachRelY = -1;
    std::int32_t m_AttachBoxW = 0;
    std::int32_t m_AttachBoxH = 0;
    pu::sdl2::TextureHandle::Ref m_ImageTex;
    pu::sdl2::Texture m_ImagePendingTex = nullptr;
    pu::sdl2::Texture m_ImageFailedTex = nullptr;
    ImageState m_ImageState = ImageState::Pending;
    std::string m_ImageMxc;
    std::int32_t m_ImageRelY = -1;
    std::int32_t m_ImageBoxW = 0;
    std::int32_t m_ImageBoxH = 0;
    std::int32_t m_ImageDestW = 0;
    std::int32_t m_ImageDestH = 0;
    // Cleared by the destructor so an in-flight media callback (posted to the
    // UI thread) can detect that this row is gone.
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
    std::int32_t m_LinesRelY = -1;
    std::int32_t m_LineH = 0;
    std::int32_t m_ReactionsRelY = -1;
};

// Room member entry: avatar, display name, user id and power/role chip.
class MemberRow : public Row {
public:
    MemberRow(std::int32_t Width, const Matrix::RoomMember &Member, Matrix::MediaLoader *Media);
    ~MemberRow();
    void OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) override;

private:
    Matrix::RoomMember m_Member;
    Avatar::Ref m_Avatar;
    pu::sdl2::Texture m_NameTex = nullptr;
    pu::sdl2::Texture m_IdTex = nullptr;
    pu::sdl2::Texture m_ChipTex = nullptr;
    pu::ui::Color m_ChipBg = Theme::Border;
};

} // namespace NxMatrix::Ui