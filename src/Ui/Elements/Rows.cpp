// Row elements: every string is measured and rasterized once (constructor or
// setter), cached as a texture and blitted per frame - text APIs are never
// called from the render loop.
#include "NxMatrix/Ui/Elements/Rows.hpp"

#include "NxMatrix/Ui/Elements/ImageTexture.hpp"
#include "NxMatrix/Ui/Elements/RichText.hpp"
#include "NxMatrix/Ui/Theme.hpp"
#include "NxMatrix/Util/String.hpp"
#include "NxMatrix/Util/Time.hpp"

#include <algorithm>

namespace NxMatrix::Ui {

namespace {

constexpr std::int32_t PadX = 16;
constexpr std::int32_t HeightSimple = 84;
constexpr std::int32_t HeightSimpleHint = 108;
constexpr std::int32_t HeightSection = 64;
constexpr std::int32_t HeightToggle = 84;
constexpr std::int32_t HeightRoom = 96;
constexpr std::int32_t HeightMember = 72;
// Inline image previews never grow past this (content width can be smaller).
constexpr std::int32_t MaxImagePreviewW = 320;
constexpr std::int32_t MaxImagePreviewH = 240;

pu::sdl2::Texture MakeText(const std::string &Font, const std::string &Text, const pu::ui::Color &Color) {
    if(Text.empty()) {
        return nullptr;
    }
    return pu::ui::render::RenderText(Font, Text, Color);
}

std::int32_t TexWidth(pu::sdl2::Texture Tex) {
    return Tex != nullptr ? pu::ui::render::GetTextureWidth(Tex) : 0;
}

std::int32_t TexHeight(pu::sdl2::Texture Tex) {
    return Tex != nullptr ? pu::ui::render::GetTextureHeight(Tex) : 0;
}

std::int32_t CenterY(std::int32_t Y, std::int32_t H, std::int32_t TexH) {
    return Y + (H - TexH) / 2;
}

std::int32_t RightX(std::int32_t X, std::int32_t W, std::int32_t Margin, std::int32_t TexW) {
    return X + W - Margin - TexW;
}

void DrawRowFocus(pu::ui::render::Renderer::Ref &Drawer, std::int32_t X, std::int32_t Y, std::int32_t W, std::int32_t H, bool Focused) {
    if(!Focused) {
        return;
    }
    Drawer->RenderRoundedRectangleFill(Theme::SurfaceRaised, X, Y, W, H, Theme::CornerRadius);
    Drawer->RenderRectangleFill(Theme::Accent, X, Y, 4, H);
}

// Shortens Text so its rendered width fits MaxWidth (measured at construction
// time only; UTF-8 safe).
std::string ClampToWidth(const std::string &Font, std::string Text, std::int32_t MaxWidth) {
    if(MaxWidth <= 0) {
        return "";
    }
    if(pu::ui::render::GetTextWidth(Font, Text) <= MaxWidth) {
        return Text;
    }
    while(!Text.empty() && pu::ui::render::GetTextWidth(Font, Text + "...") > MaxWidth) {
        std::size_t Cut = Text.size();
        while(Cut > 0 && (static_cast<unsigned char>(Text[Cut - 1]) & 0xC0) == 0x80) {
            Cut--;
        }
        if(Cut > 0) {
            Cut--;
        }
        Text.resize(Cut);
    }
    return Text + "...";
}

// "Today" is exactly the label FormatDayLabel gives for the current day.
std::string TimeLabel(std::int64_t EpochMs) {
    if(EpochMs <= 0) {
        return "";
    }
    const auto Day = Util::FormatDayLabel(EpochMs);
    if(Day == "Today") {
        return Util::FormatClock(EpochMs);
    }
    return Day;
}

} // namespace

// --- Row base ---

Row::Row(std::int32_t Width, std::int32_t Height) : m_Width(Width), m_Height(Height) {}

void Row::SetFocused(bool Focused) {
    m_Focused = Focused;
}

bool Row::IsFocused() const {
    return m_Focused;
}

bool Row::IsSelectable() const {
    return m_Selectable;
}

std::int32_t Row::GetX() {
    // Rows are positioned by the hosting ListWidget.
    return 0;
}

std::int32_t Row::GetY() {
    return 0;
}

std::int32_t Row::GetWidth() {
    return m_Width;
}

std::int32_t Row::GetHeight() {
    return m_Height;
}

void Row::OnInput(const std::uint64_t, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
    // Input is routed by ListWidget (focus + activation).
}

void Row::SetSelectable(bool Selectable) {
    m_Selectable = Selectable;
}

// --- SectionHeaderRow ---

SectionHeaderRow::SectionHeaderRow(std::int32_t Width, const std::string &Title) : Row(Width, HeightSection), m_Title(Title) {
    SetSelectable(false);
    m_TitleTex = MakeText(Theme::FontSmall, m_Title, Theme::TextFaint);
}

SectionHeaderRow::~SectionHeaderRow() {
    pu::ui::render::DeleteTexture(m_TitleTex);
}

void SectionHeaderRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    if(m_TitleTex != nullptr) {
        Drawer->RenderTexture(m_TitleTex, X + PadX, CenterY(Y, m_Height, TexHeight(m_TitleTex)));
    }
}

// --- SimpleRow ---

SimpleRow::SimpleRow(std::int32_t Width, const std::string &Label, const std::string &Value, const std::string &Hint)
    : Row(Width, Hint.empty() ? HeightSimple : HeightSimpleHint), m_Label(Label), m_Value(Value), m_Hint(Hint) {
    // Label and value carry user text (search previews, room topic/name).
    m_LabelTex = RenderRichText(Theme::FontBody, m_Label, m_LabelColor);
    m_HintTex = MakeText(Theme::FontTiny, m_Hint, Theme::TextFaint);
    RenderValue();
}

SimpleRow::~SimpleRow() {
    pu::ui::render::DeleteTexture(m_LabelTex);
    pu::ui::render::DeleteTexture(m_ValueTex);
    pu::ui::render::DeleteTexture(m_HintTex);
}

void SimpleRow::SetLabelColor(const pu::ui::Color &Color) {
    m_LabelColor = Color;
    pu::ui::render::DeleteTexture(m_LabelTex);
    m_LabelTex = RenderRichText(Theme::FontBody, m_Label, m_LabelColor);
}

void SimpleRow::RenderValue() {
    pu::ui::render::DeleteTexture(m_ValueTex);
    m_ValueTex = RenderRichText(Theme::FontSmall, m_Value, Theme::Accent);
}

void SimpleRow::SetValue(const std::string &Value) {
    m_Value = Value;
    RenderValue();
}

void SimpleRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    DrawRowFocus(Drawer, X, Y, m_Width, m_Height, m_Focused);
    std::int32_t LabelY = 0;
    if(m_HintTex != nullptr) {
        const auto Total = TexHeight(m_LabelTex) + 4 + TexHeight(m_HintTex);
        LabelY = Y + (m_Height - Total) / 2;
    } else {
        LabelY = CenterY(Y, m_Height, TexHeight(m_LabelTex));
    }
    if(m_LabelTex != nullptr) {
        Drawer->RenderTexture(m_LabelTex, X + PadX, LabelY);
    }
    if(m_HintTex != nullptr) {
        Drawer->RenderTexture(m_HintTex, X + PadX, LabelY + TexHeight(m_LabelTex) + 4);
    }
    if(m_ValueTex != nullptr) {
        Drawer->RenderTexture(m_ValueTex, RightX(X, m_Width, PadX, TexWidth(m_ValueTex)), CenterY(Y, m_Height, TexHeight(m_ValueTex)));
    }
}

// --- TextInputRow ---

TextInputRow::TextInputRow(std::int32_t Width, const std::string &Label, const std::string &Value, const std::string &Placeholder)
    : Row(Width, HeightSimple), m_Label(Label), m_Value(Value), m_Placeholder(Placeholder) {
    m_LabelTex = MakeText(Theme::FontSmall, m_Label, Theme::TextSecondary);
    RenderValue();
}

TextInputRow::~TextInputRow() {
    pu::ui::render::DeleteTexture(m_LabelTex);
    pu::ui::render::DeleteTexture(m_ValueTex);
}

void TextInputRow::RenderValue() {
    pu::ui::render::DeleteTexture(m_ValueTex);
    if(!m_Value.empty()) {
        m_ValueTex = MakeText(Theme::FontBody, m_Value, Theme::TextPrimary);
    } else {
        m_ValueTex = MakeText(Theme::FontBody, m_Placeholder, Theme::TextFaint);
    }
}

std::string TextInputRow::GetValue() const {
    return m_Value;
}

void TextInputRow::SetValue(const std::string &Value) {
    m_Value = Value;
    RenderValue();
}

void TextInputRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    DrawRowFocus(Drawer, X, Y, m_Width, m_Height, m_Focused);
    const auto Total = TexHeight(m_LabelTex) + 6 + TexHeight(m_ValueTex);
    const auto Top = Y + (m_Height - Total) / 2;
    if(m_LabelTex != nullptr) {
        Drawer->RenderTexture(m_LabelTex, X + PadX, Top);
    }
    if(m_ValueTex != nullptr) {
        Drawer->RenderTexture(m_ValueTex, X + PadX, Top + TexHeight(m_LabelTex) + 6);
    }
}

// --- ToggleRow ---

ToggleRow::ToggleRow(std::int32_t Width, const std::string &Label, bool Checked)
    : Row(Width, HeightToggle), m_Label(Label), m_Checked(Checked) {
    m_LabelTex = MakeText(Theme::FontBody, m_Label, Theme::TextPrimary);
    m_OnTex = MakeText(Theme::FontSmall, "ON", Theme::Background);
    m_OffTex = MakeText(Theme::FontTiny, "OFF", Theme::TextFaint);
}

ToggleRow::~ToggleRow() {
    pu::ui::render::DeleteTexture(m_LabelTex);
    pu::ui::render::DeleteTexture(m_OnTex);
    pu::ui::render::DeleteTexture(m_OffTex);
}

bool ToggleRow::IsChecked() const {
    return m_Checked;
}

void ToggleRow::SetChecked(bool Checked) {
    m_Checked = Checked;
}

void ToggleRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    DrawRowFocus(Drawer, X, Y, m_Width, m_Height, m_Focused);
    if(m_LabelTex != nullptr) {
        Drawer->RenderTexture(m_LabelTex, X + PadX, CenterY(Y, m_Height, TexHeight(m_LabelTex)));
    }
    pu::sdl2::Texture ChipTex = m_Checked ? m_OnTex : m_OffTex;
    const auto ChipW = TexWidth(ChipTex) + 24;
    const auto ChipH = TexHeight(ChipTex) + 10;
    const auto ChipX = RightX(X, m_Width, PadX, ChipW);
    const auto ChipY = CenterY(Y, m_Height, ChipH);
    Drawer->RenderRoundedRectangleFill(m_Checked ? Theme::Accent : Theme::Border, ChipX, ChipY, ChipW, ChipH, ChipH / 2);
    if(ChipTex != nullptr) {
        Drawer->RenderTexture(ChipTex, ChipX + (ChipW - TexWidth(ChipTex)) / 2, ChipY + (ChipH - TexHeight(ChipTex)) / 2);
    }
}

// --- RoomRow ---

RoomRow::RoomRow(std::int32_t Width, const Matrix::RoomSummary &Room, Matrix::MediaLoader *Media) : Row(Width, HeightRoom), m_Room(Room) {
    m_Avatar = Avatar::New(16, 16, 64);
    m_Avatar->SetUser(Room.RoomId, Matrix::RoomDisplayName(Room), Room.AvatarMxc, Media);
    RebuildTextures();
}

RoomRow::~RoomRow() {
    pu::ui::render::DeleteTexture(m_NameTex);
    pu::ui::render::DeleteTexture(m_PreviewTex);
    pu::ui::render::DeleteTexture(m_TimeTex);
    pu::ui::render::DeleteTexture(m_UnreadTex);
    pu::ui::render::DeleteTexture(m_E2eeTex);
    pu::ui::render::DeleteTexture(m_InviteTex);
}

void RoomRow::Update(const Matrix::RoomSummary &Room) {
    m_Room = Room;
    RebuildTextures();
}

void RoomRow::RebuildTextures() {
    pu::ui::render::DeleteTexture(m_NameTex);
    pu::ui::render::DeleteTexture(m_PreviewTex);
    pu::ui::render::DeleteTexture(m_TimeTex);
    pu::ui::render::DeleteTexture(m_UnreadTex);
    pu::ui::render::DeleteTexture(m_E2eeTex);
    pu::ui::render::DeleteTexture(m_InviteTex);

    const std::string Time = TimeLabel(m_Room.LastActivityTs);
    m_TimeTex = MakeText(Theme::FontTiny, Time, Theme::TextFaint);
    // No unread pill on spaces: the spec suggests ignoring notification
    // counts on space-rooms (their timeline is not rendered).
    if(m_Room.NotificationCount > 0 && !m_Room.IsSpace) {
        m_UnreadTex = MakeText(Theme::FontSmall, std::to_string(m_Room.NotificationCount), Theme::Background);
    }
    if(m_Room.IsEncrypted) {
        m_E2eeTex = MakeText(Theme::FontTiny, "E2EE", Theme::TextPrimary);
    }
    if(m_Room.Membership == Matrix::MembershipState::Invite) {
        m_InviteTex = MakeText(Theme::FontTiny, "INVITE", Theme::Background);
    }

    // Right column reserves room for the time/pill/chips stack.
    const auto ChipsW = TexWidth(m_E2eeTex) + (m_E2eeTex != nullptr && m_InviteTex != nullptr ? 6 : 0) + TexWidth(m_InviteTex);
    const auto PillW = TexWidth(m_UnreadTex) + 12;
    const auto RightW = std::max({TexWidth(m_TimeTex), ChipsW, PillW});
    const auto TextX = 96; // avatar (64) + margins
    const auto NameMax = m_Width - TextX - PadX - TexWidth(m_TimeTex) - 8;
    const auto PreviewMax = m_Width - TextX - PadX - RightW - 8;

    m_NameTex = MakeText(Theme::FontBody, ClampToWidth(Theme::FontBody, Matrix::RoomDisplayName(m_Room), NameMax), Theme::TextPrimary);

    // A space groups rooms, it is not a chat: joined spaces show their child
    // count instead of a message preview. Not-joined rows (space view +
    // /hierarchy) keep the explicit preview their builder set.
    std::string BasePreview = m_Room.LastMessagePreview;
    if(m_Room.IsSpace) {
        if(m_Room.Membership == Matrix::MembershipState::Join) {
            BasePreview = std::to_string(m_Room.SpaceChildCount) + (m_Room.SpaceChildCount == 1 ? " room" : " rooms");
        } else if(m_Room.Membership == Matrix::MembershipState::Invite) {
            BasePreview = "Space invitation";
        }
    }

    // Previews carry user text (possibly emoji): wrap to the single available
    // line, ellipsize when it overflows, then render emoji-aware.
    std::string Preview;
    if(PreviewMax > 0) {
        const auto Wrapped = WrapRichText(Theme::FontSmall, BasePreview, PreviewMax);
        if(!Wrapped.empty()) {
            Preview = Wrapped[0];
            if(Wrapped.size() > 1) {
                const auto Ellipsized = WrapRichText(Theme::FontSmall, Preview + "...", PreviewMax);
                if(!Ellipsized.empty()) {
                    Preview = Ellipsized[0];
                }
            }
        }
    }
    m_PreviewTex = RenderRichText(Theme::FontSmall, Preview, Theme::TextSecondary, PreviewMax);
}

void RoomRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    DrawRowFocus(Drawer, X, Y, m_Width, m_Height, m_Focused);
    m_Avatar->OnRender(Drawer, X + 16, Y + (m_Height - 64) / 2);

    const auto TextX = X + 96;
    if(m_NameTex != nullptr) {
        Drawer->RenderTexture(m_NameTex, TextX, Y + 12);
    }
    if(m_PreviewTex != nullptr) {
        Drawer->RenderTexture(m_PreviewTex, TextX, Y + 54);
    }
    if(m_TimeTex != nullptr) {
        Drawer->RenderTexture(m_TimeTex, RightX(X, m_Width, PadX, TexWidth(m_TimeTex)), Y + 14);
    }

    auto NextRightY = Y + 14 + TexHeight(m_TimeTex) + 2;
    if(m_UnreadTex != nullptr) {
        const auto PillW = TexWidth(m_UnreadTex) + 12;
        const auto PillH = TexHeight(m_UnreadTex) + 6;
        const auto PillX = RightX(X, m_Width, PadX, PillW);
        Drawer->RenderRoundedRectangleFill(Theme::Accent, PillX, NextRightY, PillW, PillH, PillH / 2);
        Drawer->RenderTexture(m_UnreadTex, PillX + 6, NextRightY + 3);
        NextRightY += PillH + 4;
    }

    // Chips stack bottom-right under the time/pill column.
    auto ChipX =
        RightX(X, m_Width, PadX, TexWidth(m_E2eeTex) + TexWidth(m_InviteTex) + (m_E2eeTex != nullptr && m_InviteTex != nullptr ? 6 : 0));
    const auto ChipY = Y + m_Height - 12 - TexHeight(m_E2eeTex != nullptr ? m_E2eeTex : m_InviteTex) - 8;
    auto DrawChip = [&](pu::sdl2::Texture Tex, const pu::ui::Color &Bg) {
        if(Tex == nullptr) {
            return;
        }
        const auto W = TexWidth(Tex) + 12;
        const auto H = TexHeight(Tex) + 8;
        Drawer->RenderRoundedRectangleFill(Bg, ChipX, ChipY, W, H, 6);
        Drawer->RenderTexture(Tex, ChipX + 6, ChipY + 4);
        ChipX += W + 6;
    };
    DrawChip(m_E2eeTex, Theme::AccentDim);
    DrawChip(m_InviteTex, Theme::Warning);
}

// --- MessageRow ---

MessageRow::MessageRow(std::int32_t Width, const Matrix::Event &Event, const std::string &SenderName, bool ShowSender, bool OwnMessage,
                       const std::vector<Matrix::ReactionGroup> &Reactions, const std::string &ReplySender, const std::string &ReplyPreview,
                       Matrix::MediaLoader *Media)
    : Row(Width, 0), m_Event(Event), m_SenderName(SenderName), m_ReplySender(ReplySender), m_ReplyPreview(ReplyPreview),
      m_Reactions(Reactions), m_ShowSender(ShowSender), m_OwnMessage(OwnMessage) {
    const auto PlainBody = Matrix::GetMessageBody(Event);
    // Image/video/audio/file/sticker messages render as a single labelled
    // chip (the marker already carries name and size); their raw body is
    // just the filename, so it is not repeated underneath.
    const auto Marker = Matrix::AttachmentMarker(Event);
    if(Event.IsRedacted) {
        m_Body = "Message deleted";
        m_BodyColor = Theme::TextFaint;
    } else if(Event.IsDecryptionFailed) {
        m_Body = "Unable to decrypt this message";
        m_BodyColor = Theme::Danger;
    } else if(Marker.empty()) {
        m_Body = !PlainBody.empty() ? PlainBody : Matrix::EventDisplayText(Event, "");
        if(Event.IsLocalEcho && !m_Body.empty()) {
            m_Body += " ...";
        }
    }
    m_TimeText = TimeLabel(Event.Timestamp);

    // Bubble geometry: avatar column for other people's messages, own
    // messages shifted 120px to the right.
    const bool WithAvatar = m_ShowSender && !m_OwnMessage;
    if(WithAvatar) {
        m_Avatar = Avatar::New(8, 8, 40);
        m_Avatar->SetUser(Event.Sender, SenderName, "", Media);
    }
    m_BubbleRelX = (WithAvatar ? 56 : 8) + (m_OwnMessage ? 120 : 0);
    m_BubbleW = std::max(240, m_Width - m_BubbleRelX - 8);
    const auto ContentX = m_BubbleRelX + 16;
    const auto ContentW = m_BubbleW - 32;

    std::int32_t Y = 12;
    if(m_ShowSender && !m_SenderName.empty()) {
        m_SenderTex = MakeText(Theme::FontSmall, m_SenderName, Theme::Accent);
        m_TimeTex = MakeText(Theme::FontTiny, m_TimeText, Theme::TextFaint);
        m_SenderRelY = Y;
        Y += std::max(TexHeight(m_SenderTex), TexHeight(m_TimeTex)) + 6;
    }
    if(!m_ReplyPreview.empty() || !m_ReplySender.empty()) {
        // Quote block: recessed panel with a vertical accent bar, the quoted
        // author on top and the collapsed quoted text underneath.
        constexpr std::int32_t ReplyPadX = 16; // bar (4) + gap (12)
        constexpr std::int32_t ReplyPadY = 6;
        constexpr std::int32_t ReplyLineGap = 3;
        const auto ReplyTextW = ContentW - ReplyPadX - 8;
        if(!m_ReplySender.empty()) {
            m_ReplyHeadTex =
                MakeText(Theme::FontTiny, ClampToWidth(Theme::FontTiny, "Reply to " + m_ReplySender, ReplyTextW), Theme::Accent);
        }
        if(!m_ReplyPreview.empty()) {
            m_ReplyTex = MakeText(Theme::FontTiny, ClampToWidth(Theme::FontTiny, m_ReplyPreview, ReplyTextW), Theme::TextSecondary);
        }
        const bool HaveBoth = m_ReplyHeadTex != nullptr && m_ReplyTex != nullptr;
        m_ReplyBlockW = ContentW;
        m_ReplyBlockH = 2 * ReplyPadY + TexHeight(m_ReplyHeadTex) + TexHeight(m_ReplyTex) + (HaveBoth ? ReplyLineGap : 0);
        m_ReplyRelY = Y;
        Y += m_ReplyBlockH + 6;
    }
    // Inline preview for image/sticker attachments. The row height is fixed at
    // construction, so the box is reserved now (aspect from the declared size)
    // and only its pixels arrive later.
    const auto Image = Matrix::GetAttachmentImage(Event);
    if(!Image.MxcUrl.empty() && Media != nullptr) {
        std::int32_t SrcW = Image.Width;
        std::int32_t SrcH = Image.Height;
        if(SrcW <= 0 || SrcH <= 0) {
            SrcW = MaxImagePreviewW;
            SrcH = MaxImagePreviewH;
        }
        const double Scale = std::min(
            1.0, std::min(static_cast<double>(std::min(ContentW, MaxImagePreviewW)) / SrcW, static_cast<double>(MaxImagePreviewH) / SrcH));
        m_ImageBoxW = std::max(24, static_cast<std::int32_t>(SrcW * Scale));
        m_ImageBoxH = std::max(24, static_cast<std::int32_t>(SrcH * Scale));
        m_ImageRelY = Y;
        Y += m_ImageBoxH + 6;
        m_ImageMxc = Image.MxcUrl;
        m_ImagePendingTex = MakeText(Theme::FontTiny, "Loading image...", Theme::TextFaint);
        m_ImageFailedTex = MakeText(Theme::FontTiny, "Preview unavailable", Theme::TextFaint);

        auto OnBytes = [Flag = m_Alive, this, Requested = Image.MxcUrl](bool Success, const ByteArray &Data) {
            // MediaLoader delivers on the UI thread; skip when this row was
            // rebuilt away while the request was in flight.
            if(!*Flag || Requested != m_ImageMxc) {
                return;
            }
            m_ImageTex = Success ? DecodeImageTexture(Data) : nullptr;
            if(m_ImageTex == nullptr) {
                m_ImageState = ImageState::Failed;
                return;
            }
            const auto TexW = pu::ui::render::GetTextureWidth(m_ImageTex->Get());
            const auto TexH = pu::ui::render::GetTextureHeight(m_ImageTex->Get());
            const double Fit = std::min(1.0, std::min(static_cast<double>(m_ImageBoxW) / TexW, static_cast<double>(m_ImageBoxH) / TexH));
            m_ImageDestW = std::max(1, static_cast<std::int32_t>(TexW * Fit));
            m_ImageDestH = std::max(1, static_cast<std::int32_t>(TexH * Fit));
            m_ImageState = ImageState::Ready;
        };
        if(Image.Encrypted) {
            Media->LoadEncryptedMxc(Image.MxcUrl, Image.EncryptedFile, OnBytes);
        } else {
            Media->LoadMxc(Image.MxcUrl, m_ImageBoxW, m_ImageBoxH, OnBytes);
        }
    }

    if(!Marker.empty()) {
        m_AttachTex = MakeText(Theme::FontTiny, ClampToWidth(Theme::FontTiny, Marker, ContentW - 16), Theme::Accent);
        m_AttachRelY = Y;
        m_AttachBoxW = TexWidth(m_AttachTex) + 16;
        m_AttachBoxH = TexHeight(m_AttachTex) + 8;
        Y += m_AttachBoxH + 4;
    }

    if(!m_Body.empty()) {
        m_WrappedLines = WrapRichText(Theme::FontTiny, m_Body, ContentW);
    }
    m_LinesRelY = m_WrappedLines.empty() ? -1 : Y;
    for(const auto &Line : m_WrappedLines) {
        auto Tex = RenderRichText(Theme::FontTiny, Line, m_BodyColor, ContentW);
        // Uniform line height across the whole body: text metrics plus the
        // actual texture height (emoji cells can differ slightly).
        std::int32_t W = 0;
        std::int32_t H = 0;
        pu::ui::render::GetTextDimensions(Theme::FontTiny, Line, W, H);
        m_LineH = std::max({m_LineH, H, TexHeight(Tex)});
        m_LineTexs.push_back(Tex);
    }
    if(!m_WrappedLines.empty()) {
        const auto Count = static_cast<std::int32_t>(m_LineTexs.size());
        Y += m_LineH * Count + 2 * (Count - 1);
    }

    if(!m_Reactions.empty()) {
        m_ReactionsRelY = Y + 6;
        std::int32_t RX = 0;
        std::int32_t RY = 0;
        std::int32_t RowH = 0;
        std::int32_t MaxH = 0;
        for(const auto &Reaction : m_Reactions) {
            ReactionChipTex Chip;
            Chip.Tex = RenderRichText(Theme::FontTiny, Reaction.Key, Theme::TextPrimary);
            Chip.W = TexWidth(Chip.Tex) + 16;
            Chip.H = TexHeight(Chip.Tex) + 8;
            Chip.Mine = Reaction.Mine;
            if(RX > 0 && RX + Chip.W > ContentW) {
                RX = 0;
                RY += RowH + 6;
                RowH = 0;
            }
            Chip.X = RX;
            Chip.Y = RY;
            RX += Chip.W + 6;
            RowH = std::max(RowH, Chip.H);
            MaxH = std::max(MaxH, RY + Chip.H);
            m_ReactionChips.push_back(Chip);
        }
        Y = m_ReactionsRelY + MaxH + 6;
    }

    m_BubbleH = Y + 12;
    m_Height = m_BubbleH + 16;
}

MessageRow::~MessageRow() {
    *m_Alive = false;
    pu::ui::render::DeleteTexture(m_SenderTex);
    pu::ui::render::DeleteTexture(m_TimeTex);
    pu::ui::render::DeleteTexture(m_ReplyHeadTex);
    pu::ui::render::DeleteTexture(m_ReplyTex);
    pu::ui::render::DeleteTexture(m_AttachTex);
    pu::ui::render::DeleteTexture(m_ImagePendingTex);
    pu::ui::render::DeleteTexture(m_ImageFailedTex);
    for(auto &Tex : m_LineTexs) {
        pu::ui::render::DeleteTexture(Tex);
    }
    for(auto &Chip : m_ReactionChips) {
        pu::ui::render::DeleteTexture(Chip.Tex);
    }
}

void MessageRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    DrawRowFocus(Drawer, X, Y, m_Width, m_Height, m_Focused);
    const auto BubbleX = X + m_BubbleRelX;
    const auto BubbleY = Y + 8;
    Drawer->RenderRoundedRectangleFill(m_OwnMessage ? Theme::BubbleOwn : Theme::BubbleOther, BubbleX, BubbleY, m_BubbleW, m_BubbleH,
                                       Theme::CornerRadius);
    const auto ContentX = BubbleX + 16;

    if(m_SenderRelY >= 0) {
        if(m_SenderTex != nullptr) {
            Drawer->RenderTexture(m_SenderTex, ContentX, BubbleY + m_SenderRelY);
        }
        if(m_TimeTex != nullptr) {
            Drawer->RenderTexture(m_TimeTex, BubbleX + m_BubbleW - 16 - TexWidth(m_TimeTex),
                                  BubbleY + m_SenderRelY + (TexHeight(m_SenderTex) - TexHeight(m_TimeTex)) / 2);
        }
    }
    if(m_ReplyRelY >= 0 && m_ReplyBlockH > 0) {
        const auto BlockY = BubbleY + m_ReplyRelY;
        Drawer->RenderRoundedRectangleFill(Theme::Surface, ContentX, BlockY, m_ReplyBlockW, m_ReplyBlockH, 6);
        Drawer->RenderRectangleFill(Theme::Accent, ContentX, BlockY, 4, m_ReplyBlockH);
        auto LineY = BlockY + 6;
        if(m_ReplyHeadTex != nullptr) {
            Drawer->RenderTexture(m_ReplyHeadTex, ContentX + 16, LineY);
            LineY += TexHeight(m_ReplyHeadTex) + 3;
        }
        if(m_ReplyTex != nullptr) {
            Drawer->RenderTexture(m_ReplyTex, ContentX + 16, LineY);
        }
    }
    if(m_ImageRelY >= 0) {
        const auto BoxY = BubbleY + m_ImageRelY;
        Drawer->RenderRoundedRectangleFill(Theme::Surface, ContentX, BoxY, m_ImageBoxW, m_ImageBoxH, 6);
        if(m_ImageTex != nullptr) {
            Drawer->RenderTexture(m_ImageTex->Get(), ContentX + (m_ImageBoxW - m_ImageDestW) / 2, BoxY + (m_ImageBoxH - m_ImageDestH) / 2,
                                  pu::ui::render::TextureRenderOptions({}, m_ImageDestW, m_ImageDestH, {}, {}, {}));
        } else {
            const auto Text = (m_ImageState == ImageState::Failed) ? m_ImageFailedTex : m_ImagePendingTex;
            if(Text != nullptr) {
                Drawer->RenderTexture(Text, ContentX + (m_ImageBoxW - TexWidth(Text)) / 2, BoxY + (m_ImageBoxH - TexHeight(Text)) / 2);
            }
        }
        Drawer->RenderRoundedRectangle(Theme::Border, ContentX, BoxY, m_ImageBoxW, m_ImageBoxH, 6);
    }
    if(m_AttachRelY >= 0 && m_AttachTex != nullptr) {
        const auto ChipY = BubbleY + m_AttachRelY;
        Drawer->RenderRoundedRectangleFill(Theme::SurfaceRaised, ContentX, ChipY, m_AttachBoxW, m_AttachBoxH, 6);
        Drawer->RenderTexture(m_AttachTex, ContentX + 8, ChipY + 4);
    }
    if(m_LinesRelY >= 0) {
        auto LineY = BubbleY + m_LinesRelY;
        for(const auto &Tex : m_LineTexs) {
            if(Tex != nullptr) {
                Drawer->RenderTexture(Tex, ContentX, LineY);
            }
            LineY += m_LineH + 2;
        }
    }
    if(m_ReactionsRelY >= 0) {
        const auto ReactY = BubbleY + m_ReactionsRelY;
        for(const auto &Chip : m_ReactionChips) {
            const auto ChipX = ContentX + Chip.X;
            const auto ChipY = ReactY + Chip.Y;
            Drawer->RenderRoundedRectangleFill(Chip.Mine ? Theme::Border : Theme::Surface, ChipX, ChipY, Chip.W, Chip.H, 6);
            if(Chip.Tex != nullptr) {
                Drawer->RenderTexture(Chip.Tex, ChipX + (Chip.W - TexWidth(Chip.Tex)) / 2, ChipY + (Chip.H - TexHeight(Chip.Tex)) / 2);
            }
        }
    }
    if(m_Avatar != nullptr) {
        m_Avatar->OnRender(Drawer, X + 8, Y + 16);
    }
}

// --- MemberRow ---

MemberRow::MemberRow(std::int32_t Width, const Matrix::RoomMember &Member, Matrix::MediaLoader *Media)
    : Row(Width, HeightMember), m_Member(Member) {
    m_Avatar = Avatar::New(16, 12, 48);
    m_Avatar->SetUser(Member.UserId, Member.DisplayName, Member.AvatarMxc, Media);
    m_NameTex = MakeText(Theme::FontBody, !Member.DisplayName.empty() ? Member.DisplayName : Member.UserId, Theme::TextPrimary);
    m_IdTex = MakeText(Theme::FontTiny, Member.UserId, Theme::TextFaint);
    std::string Chip;
    if(Member.Membership == Matrix::MembershipState::Invite) {
        Chip = "INVITED";
        m_ChipBg = Theme::Warning;
    } else if(Member.PowerLevel == Matrix::PowerLevels::InfiniteLevel) {
        Chip = "CREATOR"; // room v12+ immutable creator level (spec §Permissions)
        m_ChipBg = Theme::Accent;
    } else if(Member.PowerLevel >= 100) {
        Chip = "OWNER";
        m_ChipBg = Theme::Accent;
    } else if(Member.PowerLevel >= 50) {
        Chip = "ADMIN";
        m_ChipBg = Theme::Success;
    }
    m_ChipTex = MakeText(Theme::FontTiny, Chip, Theme::Background);
}

MemberRow::~MemberRow() {
    pu::ui::render::DeleteTexture(m_NameTex);
    pu::ui::render::DeleteTexture(m_IdTex);
    pu::ui::render::DeleteTexture(m_ChipTex);
}

void MemberRow::OnRender(pu::ui::render::Renderer::Ref &Drawer, const std::int32_t X, const std::int32_t Y) {
    DrawRowFocus(Drawer, X, Y, m_Width, m_Height, m_Focused);
    m_Avatar->OnRender(Drawer, X + 16, Y + 12);
    if(m_NameTex != nullptr) {
        Drawer->RenderTexture(m_NameTex, X + 80, Y + 8);
    }
    if(m_IdTex != nullptr) {
        Drawer->RenderTexture(m_IdTex, X + 80, Y + 44);
    }
    if(m_ChipTex != nullptr) {
        const auto ChipW = TexWidth(m_ChipTex) + 16;
        const auto ChipH = TexHeight(m_ChipTex) + 8;
        const auto ChipX = RightX(X, m_Width, PadX, ChipW);
        const auto ChipY = CenterY(Y, m_Height, ChipH);
        Drawer->RenderRoundedRectangleFill(m_ChipBg, ChipX, ChipY, ChipW, ChipH, 6);
        Drawer->RenderTexture(m_ChipTex, ChipX + 8, ChipY + 4);
    }
}

} // namespace NxMatrix::Ui