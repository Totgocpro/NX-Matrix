#pragma once
#include <pu/Plutonium>

#include <cstddef>
#include <vector>

namespace NxMatrix::Ui {

// Screen stack over the Plutonium Application: Push shows a screen, Pop
// (B button) restores the previous one with its state intact.
class Navigator {
public:
    explicit Navigator(pu::ui::Application &App);

    void Push(pu::ui::Layout::Ref Screen);
    void Pop();
    void ReplaceRoot(pu::ui::Layout::Ref Screen);

    bool CanPop() const;
    std::size_t GetDepth() const;
    pu::ui::Application &GetApp();

private:
    pu::ui::Application &m_App;
    std::vector<pu::ui::Layout::Ref> m_Stack;
};

} // namespace NxMatrix::Ui