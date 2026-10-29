#pragma once
#include "NxMatrix/E2ee/E2eeManager.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Matrix/MediaLoader.hpp"
#include "NxMatrix/Matrix/Session.hpp"
#include "NxMatrix/Matrix/SyncEngine.hpp"
#include "NxMatrix/Store/SecureStore.hpp"
#include "NxMatrix/Store/Vault.hpp"
#include "NxMatrix/Ui/Navigator.hpp"

#include <memory>

namespace NxMatrix::Ui {

// Wires the application together: SD persistence, Matrix session, E2EE, sync
// engine and navigation. Owned by NxMatrixApp, shared with every screen.
class AppContext {
public:
    explicit AppContext(pu::ui::Application &App);

    Store::SecureStore Store;
    Store::Vault Vault;
    Matrix::Client Api;
    E2ee::E2eeManager E2ee;
    Matrix::Session State;
    Matrix::SyncEngine Sync;
    Matrix::MediaLoader Media;
    Navigator Nav;

    Store::AccountData Account;   // in-memory copy of the sealed account data
    Store::PublicProfile Profile; // the only clear-text state (user id + avatar)

    // Re-exports the E2EE state and seals AccountData to the SD card.
    void PersistAccount();
    // Configures the API client, seeds E2EE and starts the sync loop from
    // Account (call once the vault is unlocked and Account is populated).
    void StartSession();
    // Seals the current room-list snapshot to the SD card so the next launch
    // can paint it before /sync answers.
    void SaveRoomCache();
};

} // namespace NxMatrix::Ui