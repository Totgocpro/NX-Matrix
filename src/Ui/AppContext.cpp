// AppContext wiring: member order in the header fixes construction order
// (E2ee/State/Sync/Media all reference Api/Store/Vault).
#include "NxMatrix/Ui/AppContext.hpp"

namespace NxMatrix::Ui {

AppContext::AppContext(pu::ui::Application &App) : E2ee(Api), State(E2ee), Sync(Api, State, E2ee), Media(Api, Store, Vault), Nav(App) {}

void AppContext::PersistAccount() {
    Account.OlmState = E2ee.ExportState();
    // Small sealed file (a few KB of pickles + tokens); written synchronously
    // so a crash right after login or key changes cannot lose the session.
    Store.SaveAccount(Vault, Account);
}

void AppContext::StartSession() {
    Api.Configure(Account.HomeserverUrl);
    Api.SetSession(Account.AccessToken, Account.DeviceId, Account.UserId);
    State.SetOwnUserId(Account.UserId);
    E2ee.SetIdentity(Account.UserId, Account.DeviceId);
    E2ee.SetMasterKey(Vault.GetMasterKey());
    E2ee.ImportState(Account.OlmState);
    Sync.Start(Account.SyncNextBatch);
}

} // namespace NxMatrix::Ui