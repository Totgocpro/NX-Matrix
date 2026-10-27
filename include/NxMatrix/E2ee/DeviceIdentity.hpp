#pragma once
#include <string>

namespace NxMatrix::E2ee {

// A Matrix device's long-term identity keys (unpadded base64).
struct DeviceIdentity {
    std::string UserId;
    std::string DeviceId;
    std::string Ed25519Key;
    std::string Curve25519Key;
};

} // namespace NxMatrix::E2ee