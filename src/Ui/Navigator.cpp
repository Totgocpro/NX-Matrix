// Screen stack over the Plutonium Application: every mutation loads the new
// top so the visible layout always matches the stack.
#include "NxMatrix/Ui/Navigator.hpp"

namespace NxMatrix::Ui {

Navigator::Navigator(pu::ui::Application &App) : m_App(App) {}

void Navigator::Push(pu::ui::Layout::Ref Screen) {
    m_Stack.push_back(Screen);
    m_App.LoadLayout(Screen);
}

void Navigator::Pop() {
    // The root screen is never popped.
    if(m_Stack.size() <= 1) {
        return;
    }
    m_Stack.pop_back();
    m_App.LoadLayout(m_Stack.back());
}

void Navigator::ReplaceRoot(pu::ui::Layout::Ref Screen) {
    m_Stack.clear();
    Push(Screen);
}

bool Navigator::CanPop() const {
    return m_Stack.size() > 1;
}

std::size_t Navigator::GetDepth() const {
    return m_Stack.size();
}

pu::ui::Application &Navigator::GetApp() {
    return m_App;
}

} // namespace NxMatrix::Ui