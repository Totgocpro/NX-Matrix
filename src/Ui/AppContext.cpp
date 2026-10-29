// AppContext wiring: member order in the header fixes construction order
// (E2ee/State/Sync/Media all reference Api/Store/Vault).
#include "NxMatrix/Ui/AppContext.hpp"

#include "NxMatrix/Util/Json.hpp"

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

    // Restore the sealed room list cached by the previous run. The cache is
    // per account: a leftover file from another login is ignored.
    nlohmann::json Cache;
    bool HasCache = Store.LoadRoomCache(Vault, Cache) && Util::JsonGetString(Cache, "userId") == Account.UserId;
    if(HasCache) {
        State.ImportCache(Cache);
    }
    // A since token only delivers changes, so without a cache it would leave
    // the list empty: fall back to a full initial sync in that case.
    Sync.Start(HasCache ? Account.SyncNextBatch : std::string());
}

void AppContext::SaveRoomCache() {
    Store.SaveRoomCache(Vault, State.ExportCache());
}

} // namespace NxMatrix::Ui