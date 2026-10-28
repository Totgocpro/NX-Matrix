// Application bootstrap: worker pool, global input, sync callbacks and the
// initial screen selection (unlock vs homeserver picker).
#include "NxMatrix/Ui/App.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/Core/JobQueue.hpp"
#include "NxMatrix/Ui/Dialogs.hpp"
#include "NxMatrix/Ui/Layouts/HomeServerLayout.hpp"
#include "NxMatrix/Ui/Layouts/UnlockLayout.hpp"
#include "NxMatrix/Ui/Theme.hpp"

#include <thread>

namespace NxMatrix::Ui {

void NxMatrixApp::OnLoad() {
    Core::JobQueue::Instance().Start(3);
    m_Context = std::make_shared<AppContext>(*this);

    // Global input on top of whatever screen is loaded.
    this->SetOnInput([this](const std::uint64_t KeysDown, const std::uint64_t, const std::uint64_t, const pu::ui::TouchPoint) {
        if((KeysDown & HidNpadButton_B) && m_Context->Nav.CanPop()) {
            m_Context->Nav.Pop();
        } else if(KeysDown & HidNpadButton_Minus) {
            if(Confirm(*this, "Quit", "Close NX-Matrix?")) {
                this->Close();
            }
        }
    });

    // Worker threads post results through the Dispatcher; drain them before
    // the layouts render.
    this->AddRenderCallback([this]() { OnFrame(); });

    // Sync callbacks already run on the UI thread (SyncEngine marshals them).
    m_Context->Sync.SetOnStateChanged([this](const Matrix::SyncEngine::ConnectionState State) {
        const auto Previous = m_SyncState;
        m_SyncState = State;
        switch(State) {
        case Matrix::SyncEngine::ConnectionState::Offline:
            ShowToast(*this, "Connection lost, retrying...");
            break;
        case Matrix::SyncEngine::ConnectionState::Online:
            if(Previous == Matrix::SyncEngine::ConnectionState::Offline) {
                ShowToast(*this, "Reconnected");
            }
            break;
        case Matrix::SyncEngine::ConnectionState::Expired:
            ShowMessage(*this, "Session expired", "Please sign in again.");
            m_Context->Nav.ReplaceRoot(HomeServerLayout::New(m_Context));
            break;
        case Matrix::SyncEngine::ConnectionState::Connecting:
        case Matrix::SyncEngine::ConnectionState::Stopped:
            break;
        }
    });

    m_LastPersist = std::chrono::steady_clock::now();
    m_Context->Sync.SetOnSyncApplied([this](const Matrix::SyncSummary &) {
        // Reseal the account at most every 10 batches or 30 seconds.
        m_SyncsSincePersist++;
        const auto Now = std::chrono::steady_clock::now();
        if(m_SyncsSincePersist >= 10 || Now - m_LastPersist >= std::chrono::seconds(30)) {
            m_SyncsSincePersist = 0;
            m_LastPersist = Now;
            m_Context->PersistAccount();
        }
    });

    m_Context->Store.Initialize();
    // First launch follows the console theme; a saved preference wins.
    bool DarkMode = true;
    if(!m_Context->Store.LoadThemeDark(DarkMode)) {
        DarkMode = Theme::DetectSystemDark();
    }
    Theme::SetDark(DarkMode);
    if(m_Context->Store.HasAccount()) {
        m_Context->Store.LoadPublicProfile(m_Context->Profile);
        m_Context->Nav.ReplaceRoot(UnlockLayout::New(m_Context));
    } else {
        m_Context->Nav.ReplaceRoot(HomeServerLayout::New(m_Context));
    }
    StartProfileSync();
}

void NxMatrixApp::OnFrame() {
    Core::Dispatcher::Instance().Drain();
}

void NxMatrixApp::StartProfileSync() {
    auto Ctx = m_Context;
    Core::JobQueue::Instance().Enqueue([Ctx]() {
        // StartSession (after the vault unlock) configures the API; wait for
        // that moment, then refresh the profile best-effort. Give up quietly
        // when the user never signs in.
        for(int Waited = 0; Waited < 600 && !(Ctx->Api.IsConfigured() && Ctx->Api.HasSession()); Waited++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if(!Ctx->Api.IsConfigured() || !Ctx->Api.HasSession()) {
            return;
        }
        auto Profile = Ctx->Api.GetProfile(Ctx->Api.GetUserId());
        if(!Profile.Ok) {
            return;
        }
        Core::Dispatcher::Instance().Post([Ctx, Fetched = std::move(Profile.Value)]() {
            Ctx->Account.DisplayName = Fetched.DisplayName;
            Ctx->Account.AvatarMxc = Fetched.AvatarMxc;
            Ctx->PersistAccount();
            if(Ctx->Profile.UserId.empty()) {
                return;
            }
            Ctx->Store.SavePublicProfile(Ctx->Profile);
            if(Fetched.AvatarMxc.empty()) {
                return;
            }
            Ctx->Media.LoadMxc(Fetched.AvatarMxc, 96, 96, [Ctx](bool Success, const ByteArray &Data) {
                if(!Success) {
                    return;
                }
                Ctx->Store.SaveAvatarImage(Data);
                Ctx->Profile.AvatarFileName = "Avatar.img";
                Ctx->Store.SavePublicProfile(Ctx->Profile);
                Ctx->PersistAccount();
            });
        });
    });
}

} // namespace NxMatrix::Ui