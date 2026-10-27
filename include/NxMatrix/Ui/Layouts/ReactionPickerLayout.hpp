#pragma once
#include "NxMatrix/Ui/AppContext.hpp"
#include "NxMatrix/Ui/Elements/ListWidget.hpp"

#include <pu/Plutonium>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

// Emoji picker used before sending a reaction. The generic options dialog
// renders through the standard font (tofu for emoji), so this screen renders
// the choices with Ui::RenderRichText and the bundled color font instead.
class ReactionPickerLayout : public pu::ui::Layout {
public:
    using PickedCallback = std::function<void(const std::string &Choice)>;

    ReactionPickerLayout(std::shared_ptr<AppContext> Context, std::vector<std::string> Choices, PickedCallback OnPicked);
    PU_SMART_CTOR(ReactionPickerLayout)

private:
    std::shared_ptr<AppContext> m_Context;
    std::vector<std::string> m_Choices;
    PickedCallback m_OnPicked;
    ListWidget::Ref m_List;
};

} // namespace NxMatrix::Ui