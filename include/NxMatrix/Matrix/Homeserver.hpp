#pragma once
#include "NxMatrix/Matrix/ApiError.hpp"

#include <string>
#include <vector>

namespace NxMatrix::Matrix {

// A pre-selected homeserver offered on the login screen.
struct HomeserverPreset {
    std::string Name;
    std::string ServerName;
};

class Homeserver {
public:
    // Curated public homeservers, shown as one-tap choices.
    static const std::vector<HomeserverPreset> &GetPresets();

    // Turns user input into a bare server name: strips schemes, slashes and
    // anything after the first "/". "https://matrix.org/" -> "matrix.org".
    static std::string NormalizeServerName(const std::string &Input);

    // Resolves the client base URL: /.well-known/matrix/client first, then
    // "https://<server name>" as fallback. ServerName must be normalized.
    static Result<std::string> ResolveBaseUrl(const std::string &ServerName);
};

} // namespace NxMatrix::Matrix