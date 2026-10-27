#include "NxMatrix/Net/HttpClient.hpp"

#include "NxMatrix/Util/String.hpp"

#include <curl/curl.h>

namespace NxMatrix::Net {

namespace {

constexpr char UserAgent[] = "NX-Matrix/0.1";

std::size_t WriteBody(char *Ptr, std::size_t Size, std::size_t Count, void *User) {
    const std::size_t Total = Size * Count;
    static_cast<std::string *>(User)->append(Ptr, Total);
    return Total; // never abort: reporting a short write would surface as a transport error
}

std::size_t ReadHeader(char *Ptr, std::size_t Size, std::size_t Count, void *User) {
    const std::size_t Total = Size * Count;
    std::string Line(Ptr, Total);
    while(!Line.empty() && (Line.back() == '\n' || Line.back() == '\r')) {
        Line.pop_back();
    }
    const std::size_t Colon = Line.find(':');
    if(Colon != std::string::npos) {
        HttpHeader Header;
        Header.Name = Util::Trim(Line.substr(0, Colon));
        Header.Value = Util::Trim(Line.substr(Colon + 1));
        static_cast<std::vector<HttpHeader> *>(User)->push_back(std::move(Header));
    }
    // The "HTTP/1.1 200 OK" status line has no colon and is skipped.
    return Total;
}

const char *MethodText(HttpMethod Method) {
    switch(Method) {
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Delete:
        return "DELETE";
    case HttpMethod::Get:
        break;
    }
    return nullptr;
}

} // namespace

bool HttpResponse::TransportOk() const {
    return ErrorText.empty();
}

HttpClient &HttpClient::Instance() {
    static HttpClient Instance;
    return Instance;
}

void HttpClient::SetCaBundlePath(const std::string &Path) {
    m_CaBundlePath = Path;
}

const std::string &HttpClient::GetUserAgent() const {
    static const std::string UserAgent = UserAgent;
    return UserAgent;
}

HttpResponse HttpClient::Send(const HttpRequest &Request) const {
    HttpResponse Response;

    CURL *Handle = curl_easy_init();
    if(Handle == nullptr) {
        Response.StatusCode = 0;
        Response.ErrorText = "curl_easy_init failed";
        return Response;
    }

    curl_easy_setopt(Handle, CURLOPT_URL, Request.Url.c_str());
    curl_easy_setopt(Handle, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(Handle, CURLOPT_WRITEDATA, &Response.Body);
    curl_easy_setopt(Handle, CURLOPT_HEADERFUNCTION, ReadHeader);
    curl_easy_setopt(Handle, CURLOPT_HEADERDATA, &Response.Headers);
    curl_easy_setopt(Handle, CURLOPT_USERAGENT, UserAgent);
    curl_easy_setopt(Handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(Handle, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(Handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(Handle, CURLOPT_TIMEOUT_MS, Request.TimeoutMs);
    curl_easy_setopt(Handle, CURLOPT_CONNECTTIMEOUT_MS, Request.ConnectTimeoutMs);
    if(!m_CaBundlePath.empty()) {
        curl_easy_setopt(Handle, CURLOPT_CAINFO, m_CaBundlePath.c_str());
    }

    const char *Method = MethodText(Request.Method);
    if(Method != nullptr) {
        curl_easy_setopt(Handle, CURLOPT_CUSTOMREQUEST, Method);
    }
    if(Request.Method != HttpMethod::Get) {
        curl_easy_setopt(Handle, CURLOPT_POSTFIELDS, Request.Body.data());
        curl_easy_setopt(Handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(Request.Body.size()));
    }

    struct curl_slist *HeaderList = nullptr;
    for(const HttpHeader &Header : Request.Headers) {
        const std::string Line = Header.Name + ": " + Header.Value;
        struct curl_slist *Next = curl_slist_append(HeaderList, Line.c_str());
        if(Next != nullptr) {
            HeaderList = Next;
        }
    }
    if(HeaderList != nullptr) {
        curl_easy_setopt(Handle, CURLOPT_HTTPHEADER, HeaderList);
    }

    const CURLcode Result = curl_easy_perform(Handle);
    if(Result != CURLE_OK) {
        // Transport-level failure: no usable status code.
        Response.StatusCode = 0;
        Response.ErrorText = curl_easy_strerror(Result);
    } else {
        long StatusCode = 0;
        curl_easy_getinfo(Handle, CURLINFO_RESPONSE_CODE, &StatusCode);
        Response.StatusCode = StatusCode;
        // 4xx/5xx are valid HTTP responses: body and status delivered, no error.
    }

    if(HeaderList != nullptr) {
        curl_slist_free_all(HeaderList);
    }
    curl_easy_cleanup(Handle);
    return Response;
}

} // namespace NxMatrix::Net