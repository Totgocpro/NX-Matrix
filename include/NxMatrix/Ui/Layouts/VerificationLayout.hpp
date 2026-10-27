#pragma once
#include "NxMatrix/Ui/AppContext.hpp"

#include <pu/Plutonium>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace NxMatrix::Ui {

// One-on-one SAS verification screen (m.sas.v1): shows the emoji/decimal
// sequence for comparison with the other device, then accept/reject and the
// verified/failed outcome. Launched from Settings with the target device.
class VerificationLayout : public pu::ui::Layout {
public:
    VerificationLayout(std::shared_ptr<AppContext> Context, std::string UserId, std::string DeviceId);
    PU_SMART_CTOR(VerificationLayout)

    void Refresh();

private:
    void UpdateFromSnapshot();
    void PollSnapshot();
    void OnAccept();
    void OnReject();

    std::shared_ptr<AppContext> m_Context;
    std::string m_UserId;
    std::string m_DeviceId;
    pu::ui::elm::TextBlock::Ref m_Title;
    pu::ui::elm::TextBlock::Ref m_Status;
    std::int32_t m_LastStep = -1; // VerificationStep mirror for change detection

    pu::ui::elm::TextBlock::Ref m_Sub;     // user + device id under the title
    pu::ui::elm::TextBlock::Ref m_Decimal; // decimal SAS line
    pu::ui::elm::TextBlock::Ref m_Hint;    // bottom button hint
    pu::ui::elm::Image::Ref m_Emoji;       // emoji row (raw texture from RenderRichText)
    E2ee::VerificationSnapshot m_Snap;     // last polled copy; drives the input handlers
    std::string m_LastTransaction;
    bool m_PopArmed = false;  // terminal outcome seen, deadline set
    bool m_PopPosted = false; // auto-pop handed to the dispatcher already
    bool m_CrossSignQueued = false;
    std::chrono::steady_clock::time_point m_PopDeadline{};
};

} // namespace NxMatrix::Ui