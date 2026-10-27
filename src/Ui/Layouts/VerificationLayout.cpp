// One-on-one SAS verification screen: polls VerificationManager's snapshot
// every frame, shows the emoji/decimal sequence while the users compare, and
// drives accept/reject/confirm through the job queue (the verification calls
// block on HTTP).
#include "NxMatrix/Ui/Layouts/VerificationLayout.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/E2ee/DeviceTracker.hpp"
#include "NxMatrix/Ui/Elements/RichText.hpp"
#include "NxMatrix/Ui/Theme.hpp"

namespace NxMatrix::Ui {

namespace {

using E2ee::VerificationStep;

// Spec SAS emoji table (matrix-spec data-definitions/sas-emoji.json), UTF-8 escapes,
// index = 6-bit group of the SAS byte stream.
constexpr const char *SasEmojis[] = {
    "\U0001F436",   // 0 Dog
    "\U0001F431",   // 1 Cat
    "\U0001F981",   // 2 Lion
    "\U0001F40E",   // 3 Horse
    "\U0001F984",   // 4 Unicorn
    "\U0001F437",   // 5 Pig
    "\U0001F418",   // 6 Elephant
    "\U0001F430",   // 7 Rabbit
    "\U0001F43C",   // 8 Panda
    "\U0001F413",   // 9 Rooster
    "\U0001F427",   // 10 Penguin
    "\U0001F422",   // 11 Turtle
    "\U0001F41F",   // 12 Fish
    "\U0001F419",   // 13 Octopus
    "\U0001F98B",   // 14 Butterfly
    "\U0001F337",   // 15 Flower
    "\U0001F333",   // 16 Tree
    "\U0001F335",   // 17 Cactus
    "\U0001F344",   // 18 Mushroom
    "\U0001F30F",   // 19 Globe
    "\U0001F319",   // 20 Moon
    "\u2601\uFE0F", // 21 Cloud
    "\U0001F525",   // 22 Fire
    "\U0001F34C",   // 23 Banana
    "\U0001F34E",   // 24 Apple
    "\U0001F353",   // 25 Strawberry
    "\U0001F33D",   // 26 Corn
    "\U0001F355",   // 27 Pizza
    "\U0001F382",   // 28 Cake
    "\u2764\uFE0F", // 29 Heart
    "\U0001F600",   // 30 Smiley
    "\U0001F916",   // 31 Robot
    "\U0001F3A9",   // 32 Hat
    "\U0001F453",   // 33 Glasses
    "\U0001F527",   // 34 Spanner
    "\U0001F385",   // 35 Santa
    "\U0001F44D",   // 36 Thumbs Up
    "\u2602\uFE0F", // 37 Umbrella
    "\u231B",       // 38 Hourglass
    "\u23F0",       // 39 Clock
    "\U0001F381",   // 40 Gift
    "\U0001F4A1",   // 41 Light Bulb
    "\U0001F4D5",   // 42 Book
    "\u270F\uFE0F", // 43 Pencil
    "\U0001F4CE",   // 44 Paperclip
    "\u2702\uFE0F", // 45 Scissors
    "\U0001F512",   // 46 Lock
    "\U0001F511",   // 47 Key
    "\U0001F528",   // 48 Hammer
    "\u260E\uFE0F", // 49 Telephone
    "\U0001F3C1",   // 50 Flag
    "\U0001F682",   // 51 Train
    "\U0001F6B2",   // 52 Bicycle
    "\u2708\uFE0F", // 53 Aeroplane
    "\U0001F680",   // 54 Rocket
    "\U0001F3C6",   // 55 Trophy
    "\u26BD",       // 56 Ball
    "\U0001F3B8",   // 57 Guitar
    "\U0001F3BA",   // 58 Trumpet
    "\U0001F514",   // 59 Bell
    "\u2693",       // 60 Anchor
    "\U0001F3A7",   // 61 Headphones
    "\U0001F4C1",   // 62 Folder
    "\U0001F4CC",   // 63 Pin
};

std::string BuildEmojiText(const std::vector<int> &Indexes) {
    std::string Out;
    for(const int Index : Indexes) {
        if(Index >= 0 && static_cast<std::size_t>(Index) < sizeof(SasEmojis) / sizeof(SasEmojis[0])) {
            Out += SasEmojis[Index];
        }
    }
    return Out;
}

std::string BuildDecimalText(const std::vector<int> &Values) {
    std::string Out;
    for(const int Value : Values) {
        if(!Out.empty()) {
            Out += ' ';
        }
        Out += std::to_string(Value);
    }
    return Out;
}

bool IsTerminal(VerificationStep Step) {
    return Step == VerificationStep::Verified || Step == VerificationStep::Cancelled || Step == VerificationStep::Failed;
}

} // namespace

VerificationLayout::VerificationLayout(std::shared_ptr<AppContext> Context, std::string UserId, std::string DeviceId)
    : m_Context(std::move(Context)), m_UserId(std::move(UserId)), m_DeviceId(std::move(DeviceId)) {
    this->SetBackgroundColor(Theme::Background);

    m_Title = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 30, "Verify device");
    m_Title->SetFont(Theme::FontHeading);
    m_Title->SetColor(Theme::Accent);
    this->Add(m_Title);

    m_Sub = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 92, m_UserId + " - " + m_DeviceId);
    m_Sub->SetFont(Theme::FontSmall);
    m_Sub->SetColor(Theme::TextSecondary);
    this->Add(m_Sub);

    m_Status = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 170, "");
    m_Status->SetFont(Theme::FontBody);
    m_Status->SetColor(Theme::TextSecondary);
    this->Add(m_Status);

    m_Decimal = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, 380, "");
    m_Decimal->SetFont(Theme::FontHeading);
    m_Decimal->SetColor(Theme::Accent);
    m_Decimal->SetVisible(false);
    this->Add(m_Decimal);

    m_Hint = pu::ui::elm::TextBlock::New(Theme::ScreenMargin, Theme::ScreenHeight - 56, "A Confirm   Y Reject   B Back");
    m_Hint->SetFont(Theme::FontTiny);
    m_Hint->SetColor(Theme::TextFaint);
    this->Add(m_Hint);

    // Poll the verification state every frame; visuals are rebuilt only when
    // the step or transaction changes (see UpdateFromSnapshot).
    this->AddRenderCallback([this]() { PollSnapshot(); });

    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        if(KeysDown & HidNpadButton_A) {
            OnAccept();
        } else if(KeysDown & HidNpadButton_Y) {
            OnReject();
        }
    });

    Refresh();
}

void VerificationLayout::Refresh() {
    m_LastStep = -1;
    m_LastTransaction.clear();
    m_PopArmed = false;
    m_PopPosted = false;
    PollSnapshot();
}

void VerificationLayout::PollSnapshot() {
    m_Snap = m_Context->E2ee.GetVerification().GetSnapshot();
    if(static_cast<int>(m_Snap.Step) != m_LastStep || m_Snap.TransactionId != m_LastTransaction) {
        UpdateFromSnapshot();
    }
    if(!IsTerminal(m_Snap.Step)) {
        m_PopArmed = false;
        m_PopPosted = false;
        return;
    }
    if(!m_PopArmed) {
        m_PopArmed = true;
        m_PopDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        return;
    }
    if(!m_PopPosted && std::chrono::steady_clock::now() >= m_PopDeadline) {
        m_PopPosted = true;
        // Pop through the dispatcher: unloading this layout from inside its own
        // render callback would destroy the callback's owner mid-execution.
        auto Ctx = m_Context;
        Core::Dispatcher::Instance().Post([Ctx]() {
            if(Ctx->Nav.CanPop()) {
                Ctx->Nav.Pop();
            }
        });
    }
}

void VerificationLayout::UpdateFromSnapshot() {
    m_LastStep = static_cast<int>(m_Snap.Step);
    m_LastTransaction = m_Snap.TransactionId;

    std::string Status;
    pu::ui::Color Color = Theme::TextSecondary;
    switch(m_Snap.Step) {
    case VerificationStep::RequestReceived:
        Status = "A to confirm   Y to reject";
        Color = Theme::Accent;
        break;
    case VerificationStep::EmojisReady:
        Status = "Compare the emojis on both screens";
        Color = Theme::Accent;
        break;
    case VerificationStep::Verified:
        Status = "Verified!";
        Color = Theme::Success;
        break;
    case VerificationStep::Cancelled:
        Status = m_Snap.Error.empty() ? "Cancelled" : "Cancelled: " + m_Snap.Error;
        Color = Theme::Warning;
        break;
    case VerificationStep::Failed:
        Status = "Failed: " + (m_Snap.Error.empty() ? std::string("unknown error") : m_Snap.Error);
        Color = Theme::Danger;
        break;
    default:
        Status = "Waiting for the other device...";
        break;
    }
    m_Status->SetText(Status);
    m_Status->SetColor(Color);

    const std::string DecimalText = BuildDecimalText(m_Snap.DecimalValues);
    m_Decimal->SetText(DecimalText);
    m_Decimal->SetVisible(!DecimalText.empty());
    if(!DecimalText.empty()) {
        m_Decimal->SetX((Theme::ScreenWidth - pu::ui::render::GetTextWidth(Theme::FontHeading, DecimalText)) / 2);
    }

    if(!m_Snap.EmojiIndexes.empty()) {
        const std::string EmojiText = BuildEmojiText(m_Snap.EmojiIndexes);
        pu::sdl2::Texture Raw = RenderRichText(Theme::FontTitle, EmojiText, Theme::TextPrimary);
        if(Raw != nullptr) {
            if(m_Emoji == nullptr) {
                m_Emoji = pu::ui::elm::Image::New(Theme::ScreenMargin, 270, pu::sdl2::TextureHandle::New(Raw));
                this->Add(m_Emoji);
            } else {
                m_Emoji->SetImage(pu::sdl2::TextureHandle::New(Raw));
            }
            // No emoji font at runtime: the decimal line above stays as the text fallback.
            m_Emoji->SetX((Theme::ScreenWidth - m_Emoji->GetWidth()) / 2);
            m_Emoji->SetVisible(true);
        } else if(m_Emoji != nullptr) {
            m_Emoji->SetVisible(false);
        }
    } else if(m_Emoji != nullptr) {
        m_Emoji->SetVisible(false);
    }

    if(m_Snap.Step == VerificationStep::Verified && !m_CrossSignQueued) {
        m_CrossSignQueued = true;
        auto Ctx = m_Context;
        if(Ctx->E2ee.GetCrossSigning().IsSetUp()) {
            // Best effort: publishing the signature is what makes the freshly
            // verified device trustworthy for our other sessions as well.
            const std::string User = m_Snap.OtherUserId;
            const std::string Device = m_Snap.OtherDeviceId;
            Core::JobQueue::Instance().Enqueue([Ctx, User, Device]() {
                auto &Tracker = Ctx->E2ee.GetDevices();
                std::vector<E2ee::DeviceIdentity> Devices = Tracker.GetDevices(User);
                const E2ee::DeviceIdentity *Found = nullptr;
                for(const E2ee::DeviceIdentity &Entry : Devices) {
                    if(Entry.DeviceId == Device) {
                        Found = &Entry;
                        break;
                    }
                }
                if(Found == nullptr) {
                    Tracker.QueryKeys(Ctx->Api, {User});
                    Devices = Tracker.GetDevices(User);
                    for(const E2ee::DeviceIdentity &Entry : Devices) {
                        if(Entry.DeviceId == Device) {
                            Found = &Entry;
                            break;
                        }
                    }
                }
                if(Found != nullptr) {
                    (void)Ctx->E2ee.GetCrossSigning().SignDevice(*Found);
                }
            });
        }
    }
}

void VerificationLayout::OnAccept() {
    auto Ctx = m_Context;
    if(m_Snap.Step == VerificationStep::RequestReceived) {
        Core::JobQueue::Instance().Enqueue([Ctx]() { (void)Ctx->E2ee.GetVerification().AcceptIncoming(); });
    } else if(m_Snap.Step == VerificationStep::EmojisReady) {
        Core::JobQueue::Instance().Enqueue([Ctx]() { (void)Ctx->E2ee.GetVerification().ConfirmEmojisMatch(); });
    }
}

void VerificationLayout::OnReject() {
    auto Ctx = m_Context;
    const VerificationStep Step = m_Snap.Step;
    if(Step == VerificationStep::RequestReceived) {
        Core::JobQueue::Instance().Enqueue([Ctx]() { (void)Ctx->E2ee.GetVerification().RejectIncoming(); });
    } else if(Step == VerificationStep::EmojisReady) {
        Core::JobQueue::Instance().Enqueue([Ctx]() { (void)Ctx->E2ee.GetVerification().RejectEmojis(); });
    } else if(Step == VerificationStep::RequestSent || Step == VerificationStep::Started || Step == VerificationStep::AcceptedByUs) {
        Core::JobQueue::Instance().Enqueue([Ctx]() { Ctx->E2ee.GetVerification().Cancel(); });
    }
}

} // namespace NxMatrix::Ui