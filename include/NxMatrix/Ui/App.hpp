#pragma once
#include "NxMatrix/Ui/AppContext.hpp"

#include <pu/Plutonium>

#include <chrono>
#include <memory>

namespace NxMatrix::Ui {

// Plutonium application: owns the AppContext (session, storage, sync), pumps
// the UI-thread Dispatcher every frame and routes global input (B = back,
// + = quit).
class NxMatrixApp : public pu::ui::Application {
public:
    using Application::Application;
    PU_SMART_CTOR(NxMatrixApp)

    void OnLoad() override;

private:
    void OnFrame();
    void StartProfileSync();

    std::shared_ptr<AppContext> m_Context;
    Matrix::SyncEngine::ConnectionState m_SyncState = Matrix::SyncEngine::ConnectionState::Stopped;
    std::size_t m_SyncsSincePersist = 0;
    std::chrono::steady_clock::time_point m_LastPersist{};
};

} // namespace NxMatrix::Ui