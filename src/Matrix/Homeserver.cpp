#include "NxMatrix/Matrix/Homeserver.hpp"

#include "NxMatrix/Net/HttpClient.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/String.hpp"

namespace NxMatrix::Matrix {

const std::vector<HomeserverPreset> &Homeserver::GetPresets() {
    static const std::vector<HomeserverPreset> Presets = {
        {"Matrix.org", "matrix.org"}, {"tchncs.de", "tchncs.de"}, {"EnvS", "envs.net"},
        {"Mozilla", "mozilla.org"},   {"Gitter", "gitter.im"},    {"Nitro", "nitro.chat"},
    };
    return Presets;
}

std::string Homeserver::NormalizeServerName(const std::string &Input) {
    std::string Out = Util::Trim(Input);
    const std::size_t Scheme = Out.find("://");
    if(Scheme != std::string::npos) {
        Out = Out.substr(Scheme + 3);
    }
    const std::size_t First = Out.find_first_not_of('/');
    Out = (First == std::string::npos) ? std::string{} : Out.substr(First);
    const std::size_t Cut = Out.find_first_of("/?#");
    if(Cut != std::string::npos) {
        Out = Out.substr(0, Cut);
    }
    return Util::ToLower(Out);
}

Result<std::string> Homeserver::ResolveBaseUrl(const std::string &ServerName) {
    Net::HttpRequest Req;
    Req.Method = Net::HttpMethod::Get;
    Req.Url = "https://" + ServerName + "/.well-known/matrix/client";
    Req.Headers.push_back({"Accept", "application/json"});
    Req.TimeoutMs = 8000;
    Req.ConnectTimeoutMs = 8000;

    // Discovery must never block the caller: any transport/HTTP/shape failure
    // falls back to the conventional https://<server name>.
    const Net::HttpResponse Resp = Net::HttpClient::Instance().Send(Req);
    if(Resp.TransportOk() && Resp.StatusCode == 200) {
        const auto Parsed = nlohmann::json::parse(Resp.Body, nullptr, false);
        if(Parsed.is_object() && Parsed.contains("m.homeserver") && Parsed["m.homeserver"].is_object()) {
            std::string BaseUrl = Util::JsonGetString(Parsed["m.homeserver"], "base_url");
            while(!BaseUrl.empty() && BaseUrl.back() == '/') {
                BaseUrl.pop_back();
            }
            if(!BaseUrl.empty()) {
                return Result<std::string>::Success(std::move(BaseUrl));
            }
        }
    }
    return Result<std::string>::Success("https://" + ServerName);
}

} // namespace NxMatrix::Matrix