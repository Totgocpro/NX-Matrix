#pragma once
#include <string>
#include <vector>

namespace NxMatrix::Net {

enum class HttpMethod {
    Get,
    Post,
    Put,
    Delete,
};

struct HttpHeader {
    std::string Name;
    std::string Value;
};

struct HttpRequest {
    HttpMethod Method = HttpMethod::Get;
    std::string Url;
    std::vector<HttpHeader> Headers;
    std::string Body;
    long TimeoutMs = 15000;
    long ConnectTimeoutMs = 8000;
};

struct HttpResponse {
    long StatusCode = 0;
    std::string Body;
    std::vector<HttpHeader> Headers;
    std::string ErrorText; // empty when the transfer itself succeeded

    bool TransportOk() const;
};

// Blocking libcurl wrapper with TLS verification. All calls run on worker
// threads (Core::JobQueue); never call it from the UI thread.
class HttpClient {
public:
    static HttpClient &Instance();

    // PEM CA bundle used to verify TLS (Horizon has no system CA store).
    void SetCaBundlePath(const std::string &Path);
    const std::string &GetUserAgent() const;

    HttpResponse Send(const HttpRequest &Request) const;

private:
    HttpClient() = default;

    std::string m_CaBundlePath;
};

} // namespace NxMatrix::Net