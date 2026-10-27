#pragma once
#include "NxMatrix/E2ee/DeviceIdentity.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace NxMatrix::Matrix {
class Client;
}

namespace NxMatrix::E2ee {

class OlmAccount;
class OlmSessionManager;

// Tracks the device lists of room members (/keys/query, /keys/claim and the
// sync "device_lists" section) so room keys can be shared with everyone.
class DeviceTracker {
public:
    nlohmann::json ExportState() const;
    void ImportState(const nlohmann::json &State);

    // Users whose device list may be stale (sync device_lists.changed).
    void MarkUsersDirty(const std::vector<std::string> &UserIds);
    void MarkUserDirty(const std::string &UserId);

    // Blocking: POST /keys/query for the dirty users (or the given ones).
    bool QueryKeys(Matrix::Client &Api, const std::vector<std::string> &UserIds);

    std::vector<DeviceIdentity> GetDevices(const std::string &UserId) const;
    std::vector<DeviceIdentity> GetDevicesForUsers(const std::vector<std::string> &UserIds) const;

    // Blocking: POST /keys/claim for devices without an olm session, then
    // creates the outbound olm sessions.
    bool EnsureOlmSessions(Matrix::Client &Api, OlmAccount &Account, OlmSessionManager &Sessions,
                           const std::vector<DeviceIdentity> &Devices);

private:
    // Members whose device list is dirty or was never queried.
    std::vector<std::string> FilterStale(const std::vector<std::string> &UserIds) const;
    // Finds the owner device of a curve25519 key (key requests need the sender device).
    std::vector<DeviceIdentity> FindDevicesByCurveKey(const std::string &CurveKey) const;

    std::map<std::string, std::vector<DeviceIdentity>> m_Devices; // key: user id
    std::vector<std::string> m_DirtyUsers;

    friend class E2eeManager;
};

} // namespace NxMatrix::E2ee