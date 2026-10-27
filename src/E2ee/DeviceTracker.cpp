// Device lists via /keys/query and olm session setup via /keys/claim (both blocking).
#include "NxMatrix/E2ee/DeviceTracker.hpp"

#include "NxMatrix/E2ee/OlmAccount.hpp"
#include "NxMatrix/E2ee/OlmSessionManager.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Util/Json.hpp"

#include <set>

namespace NxMatrix::E2ee {

namespace {

// Splits "@user:server" into "server" for the /keys/query "failures" lookup.
std::string ServerNameOf(const std::string &UserId) {
    const std::size_t At = UserId.rfind(':');
    return At == std::string::npos ? UserId : UserId.substr(At + 1);
}

} // namespace

nlohmann::json DeviceTracker::ExportState() const {
    nlohmann::json State;
    State["devices"] = nlohmann::json::object();
    for(const auto &[UserId, Devices] : m_Devices) {
        nlohmann::json List = nlohmann::json::array();
        for(const DeviceIdentity &Device : Devices) {
            nlohmann::json Item;
            Item["user_id"] = Device.UserId;
            Item["device_id"] = Device.DeviceId;
            Item["ed25519"] = Device.Ed25519Key;
            Item["curve25519"] = Device.Curve25519Key;
            List.push_back(Item);
        }
        State["devices"][UserId] = List;
    }
    State["dirty"] = nlohmann::json(m_DirtyUsers);
    return State;
}

void DeviceTracker::ImportState(const nlohmann::json &State) {
    m_Devices.clear();
    m_DirtyUsers.clear();
    if(!State.is_object()) {
        return;
    }
    if(State.contains("devices") && State["devices"].is_object()) {
        const nlohmann::json &Devices = State["devices"];
        for(auto It = Devices.begin(); It != Devices.end(); ++It) {
            if(!It.value().is_array()) {
                continue;
            }
            std::vector<DeviceIdentity> List;
            for(const nlohmann::json &Item : It.value()) {
                if(!Item.is_object()) {
                    continue;
                }
                DeviceIdentity Device;
                Device.UserId = Util::JsonGetString(Item, "user_id", It.key());
                Device.DeviceId = Util::JsonGetString(Item, "device_id");
                Device.Ed25519Key = Util::JsonGetString(Item, "ed25519");
                Device.Curve25519Key = Util::JsonGetString(Item, "curve25519");
                if(!Device.DeviceId.empty() && !Device.Ed25519Key.empty() && !Device.Curve25519Key.empty()) {
                    List.push_back(std::move(Device));
                }
            }
            m_Devices[It.key()] = std::move(List);
        }
    }
    if(State.contains("dirty") && State["dirty"].is_array()) {
        for(const nlohmann::json &Item : State["dirty"]) {
            if(Item.is_string()) {
                MarkUserDirty(Item.get<std::string>());
            }
        }
    }
}

void DeviceTracker::MarkUsersDirty(const std::vector<std::string> &UserIds) {
    for(const std::string &UserId : UserIds) {
        MarkUserDirty(UserId);
    }
}

void DeviceTracker::MarkUserDirty(const std::string &UserId) {
    if(UserId.empty()) {
        return;
    }
    for(const std::string &Dirty : m_DirtyUsers) {
        if(Dirty == UserId) {
            return;
        }
    }
    m_DirtyUsers.push_back(UserId);
}

bool DeviceTracker::QueryKeys(Matrix::Client &Api, const std::vector<std::string> &UserIds) {
    const std::vector<std::string> &Targets = UserIds.empty() ? m_DirtyUsers : UserIds;
    if(Targets.empty()) {
        return true;
    }

    nlohmann::json Body;
    nlohmann::json Query = nlohmann::json::object();
    for(const std::string &UserId : Targets) {
        Query[UserId] = nlohmann::json::array(); // empty list = all devices of the user
    }
    Body["device_keys"] = Query;
    Body["timeout"] = 10000; // spec recommends 10s for remote-server waits

    auto Response = Api.QueryKeys(Body);
    if(!Response.Ok) {
        return false; // dirty flags stay set so the next call retries
    }

    std::set<std::string> FailedServers;
    if(Response.Value.contains("failures") && Response.Value["failures"].is_object()) {
        for(auto It = Response.Value["failures"].begin(); It != Response.Value["failures"].end(); ++It) {
            FailedServers.insert(It.key());
        }
    }

    const nlohmann::json Empty = nlohmann::json::object();
    const nlohmann::json &ResultKeys =
        Response.Value.contains("device_keys") && Response.Value["device_keys"].is_object() ? Response.Value["device_keys"] : Empty;

    for(const std::string &UserId : Targets) {
        if(FailedServers.count(ServerNameOf(UserId)) != 0) {
            continue; // remote server unreachable: keep the stale list and the dirty flag
        }
        const nlohmann::json &UserEntry = ResultKeys.contains(UserId) ? ResultKeys[UserId] : Empty;
        std::vector<DeviceIdentity> Devices;
        if(UserEntry.is_object()) {
            for(auto It = UserEntry.begin(); It != UserEntry.end(); ++It) {
                const nlohmann::json &Entry = It.value();
                if(!Entry.is_object()) {
                    continue;
                }
                const std::string EntryUser = Util::JsonGetString(Entry, "user_id");
                const std::string EntryDevice = Util::JsonGetString(Entry, "device_id");
                // Structural validation only: the map keys must match the identity fields
                // and both device keys must be present. Self-signature verification is
                // out of scope (documented on E2eeManager).
                if(EntryUser != UserId || EntryDevice != It.key() || !Entry.contains("keys") || !Entry["keys"].is_object()) {
                    continue;
                }
                const nlohmann::json &Keys = Entry["keys"];
                const std::string Ed25519 = Util::JsonGetString(Keys, "ed25519:" + It.key());
                const std::string Curve25519 = Util::JsonGetString(Keys, "curve25519:" + It.key());
                if(Ed25519.empty() || Curve25519.empty()) {
                    continue;
                }
                DeviceIdentity Device;
                Device.UserId = UserId;
                Device.DeviceId = It.key();
                Device.Ed25519Key = Ed25519;
                Device.Curve25519Key = Curve25519;
                Devices.push_back(std::move(Device));
            }
        }
        m_Devices[UserId] = std::move(Devices); // replace: the server response is authoritative
        for(auto It = m_DirtyUsers.begin(); It != m_DirtyUsers.end();) {
            if(*It == UserId) {
                It = m_DirtyUsers.erase(It);
            } else {
                ++It;
            }
        }
    }
    return true;
}

std::vector<DeviceIdentity> DeviceTracker::GetDevices(const std::string &UserId) const {
    auto It = m_Devices.find(UserId);
    return It == m_Devices.end() ? std::vector<DeviceIdentity>{} : It->second;
}

std::vector<DeviceIdentity> DeviceTracker::GetDevicesForUsers(const std::vector<std::string> &UserIds) const {
    std::vector<DeviceIdentity> Out;
    for(const std::string &UserId : UserIds) {
        auto It = m_Devices.find(UserId);
        if(It != m_Devices.end()) {
            Out.insert(Out.end(), It->second.begin(), It->second.end());
        }
    }
    return Out;
}

bool DeviceTracker::EnsureOlmSessions(Matrix::Client &Api, OlmAccount &Account, OlmSessionManager &Sessions,
                                      const std::vector<DeviceIdentity> &Devices) {
    // The session manager creates sessions from this account; keep the binding current.
    Sessions.m_Account = &Account;
    if(!Account.IsValid()) {
        return false; // nothing can be derived from an invalid account
    }

    std::vector<DeviceIdentity> Missing;
    for(const DeviceIdentity &Device : Devices) {
        if(Device.Curve25519Key.empty() || Device.UserId.empty() || Device.DeviceId.empty()) {
            continue;
        }
        if(!Sessions.HasSessionFor(Device.Curve25519Key)) {
            Missing.push_back(Device);
        }
    }
    if(Missing.empty()) {
        return true;
    }

    nlohmann::json Claim = nlohmann::json::object();
    for(const DeviceIdentity &Device : Missing) {
        Claim[Device.UserId][Device.DeviceId] = "signed_curve25519";
    }
    nlohmann::json Body;
    Body["one_time_keys"] = Claim;
    Body["timeout"] = 5000;

    auto Response = Api.ClaimKeys(Body);
    if(!Response.Ok) {
        return false;
    }

    const nlohmann::json Empty = nlohmann::json::object();
    const nlohmann::json &ResultKeys =
        Response.Value.contains("one_time_keys") && Response.Value["one_time_keys"].is_object() ? Response.Value["one_time_keys"] : Empty;

    bool AllCreated = true;
    for(const DeviceIdentity &Device : Missing) {
        const nlohmann::json &UserEntry = ResultKeys.contains(Device.UserId) ? ResultKeys[Device.UserId] : Empty;
        const nlohmann::json &DeviceEntry =
            UserEntry.is_object() && UserEntry.contains(Device.DeviceId) ? UserEntry[Device.DeviceId] : Empty;
        if(!DeviceEntry.is_object()) {
            AllCreated = false;
            continue;
        }
        // Response: {"signed_curve25519:<id>": {"key": "...", "signatures": {...}}} — one entry.
        bool Created = false;
        for(auto It = DeviceEntry.begin(); It != DeviceEntry.end(); ++It) {
            if(It.key().rfind("signed_curve25519:", 0) != 0) {
                continue;
            }
            std::string RawKey;
            if(It.value().is_object()) {
                RawKey = Util::JsonGetString(It.value(), "key");
            } else if(It.value().is_string()) {
                RawKey = It.value().get<std::string>(); // unsigned fallback key claims
            }
            if(RawKey.empty()) {
                continue;
            }
            const std::string KeyId = It.key().substr(std::string("signed_curve25519:").size());
            Created = Sessions.CreateOutboundSession(Device, KeyId, RawKey);
            break;
        }
        if(!Created) {
            AllCreated = false;
        }
    }
    return AllCreated;
}

std::vector<std::string> DeviceTracker::FilterStale(const std::vector<std::string> &UserIds) const {
    std::vector<std::string> Stale;
    for(const std::string &UserId : UserIds) {
        if(UserId.empty()) {
            continue;
        }
        bool Dirty = false;
        for(const std::string &DirtyUser : m_DirtyUsers) {
            if(DirtyUser == UserId) {
                Dirty = true;
                break;
            }
        }
        // Never queried == equally stale: the first room-key share needs those devices.
        if(Dirty || m_Devices.find(UserId) == m_Devices.end()) {
            Stale.push_back(UserId);
        }
    }
    return Stale;
}

std::vector<DeviceIdentity> DeviceTracker::FindDevicesByCurveKey(const std::string &CurveKey) const {
    std::vector<DeviceIdentity> Found;
    if(CurveKey.empty()) {
        return Found;
    }
    for(const auto &Pair : m_Devices) {
        for(const DeviceIdentity &Device : Pair.second) {
            if(Device.Curve25519Key == CurveKey) {
                Found.push_back(Device);
            }
        }
    }
    return Found;
}

} // namespace NxMatrix::E2ee