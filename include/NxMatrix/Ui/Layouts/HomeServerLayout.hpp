#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <memory>
#include <string>

namespace NxMatrix::Ui {

// First screen when no account is stored: pick a pre-selected homeserver or
// type another one (Horizon keyboard), then continue to login/registration.
class HomeServerLayout : public pu::ui::Layout {
public:
    explicit HomeServerLayout(std::shared_ptr<AppContext> Context);
    PU_SMART_CTOR(HomeServerLayout)

private:
    void OnServerPicked(const std::string &ServerName);
    void ResolveServer(const std::string &ServerName);
    void SetStatus(const std::string &Text, const pu::ui::Color &Color);

    std::shared_ptr<AppContext> m_Context;
    ListWidget::Ref m_List;
    pu::ui::elm::TextBlock::Ref m_Status;
};

} // namespace NxMatrix::Ui