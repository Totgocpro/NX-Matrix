#include "NxMatrix/Matrix/Client.hpp"

#include "NxMatrix/Net/HttpClient.hpp"
#include "NxMatrix/Util/Json.hpp"
#include "NxMatrix/Util/String.hpp"

#include <utility>

namespace NxMatrix::Matrix {

namespace {

using Net::HttpMethod;
using Net::HttpRequest;
using Net::HttpResponse;

constexpr long kDefaultTimeoutMs = 15000;
constexpr long kConnectTimeoutMs = 8000;
constexpr const char *kJsonContentType = "application/json";

const nlohmann::json kEmptyJson = nlohmann::json::object();

std::string Enc(const std::string &Segment) {
    return Util::UrlEncode(Segment);
}

std::string BuildUrl(const std::string &BaseUrl, const std::string &Path, const std::vector<std::pair<std::string, std::string>> &Query) {
    std::string Url = BaseUrl;
    while(!Url.empty() && Url.back() == '/') {
        Url.pop_back();
    }
    if(!Path.empty()) {
        if(Path.front() != '/') {
            Url.push_back('/');
        }
        Url += Path;
    }
    bool FirstQuery = true;
    for(const auto &Param : Query) {
        Url += FirstQuery ? '?' : '&';
        FirstQuery = false;
        Url += Util::UrlEncode(Param.first);
        Url += '=';
        Url += Util::UrlEncode(Param.second);
    }
    return Url;
}

struct RequestOpt {
    HttpMethod Method = HttpMethod::Get;
    std::string BaseUrl;
    std::string Path;
    std::vector<std::pair<std::string, std::string>> Query;
    std::string Token;
    const nlohmann::json *JsonBody = nullptr;
    std::string RawBody;
    std::string ContentType;
    long TimeoutMs = kDefaultTimeoutMs;
};

RequestOpt MakeOpt(HttpMethod Method, const std::string &BaseUrl, const std::string &Token, const std::string &Path) {
    RequestOpt Opt;
    Opt.Method = Method;
    Opt.BaseUrl = BaseUrl;
    Opt.Token = Token;
    Opt.Path = Path;
    return Opt;
}

HttpResponse Execute(const RequestOpt &Opt) {
    HttpRequest Req;
    Req.Method = Opt.Method;
    Req.Url = BuildUrl(Opt.BaseUrl, Opt.Path, Opt.Query);
    Req.TimeoutMs = Opt.TimeoutMs;
    Req.ConnectTimeoutMs = kConnectTimeoutMs;
    if(!Opt.Token.empty()) {
        Req.Headers.push_back({"Authorization", "Bearer " + Opt.Token});
    }
    if(Opt.JsonBody != nullptr) {
        Req.Body = Opt.JsonBody->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        Req.Headers.push_back({"Content-Type", kJsonContentType});
    } else {
        Req.Body = Opt.RawBody;
        if(!Opt.ContentType.empty()) {
            Req.Headers.push_back({"Content-Type", Opt.ContentType});
        }
    }
    return Net::HttpClient::Instance().Send(Req);
}

bool Is2xx(long StatusCode) {
    return StatusCode >= 200 && StatusCode <= 299;
}

// Transport failure -> ApiError.TransportError; HTTP error -> {errcode,error}
// from the body with a generic fallback when the server sent something else.
ApiError MakeError(const HttpResponse &Resp) {
    ApiError E;
    if(!Resp.TransportOk()) {
        E.TransportError = Resp.ErrorText.empty() ? std::string("connection failed") : Resp.ErrorText;
        return E;
    }
    E.StatusCode = Resp.StatusCode;
    const auto Body = nlohmann::json::parse(Resp.Body, nullptr, false);
    if(Body.is_object()) {
        E.ErrCode = Util::JsonGetString(Body, "errcode");
        E.Message = Util::JsonGetString(Body, "error");
    }
    if(E.ErrCode.empty()) {
        E.ErrCode = "M_UNKNOWN";
    }
    if(E.Message.empty()) {
        E.Message = "The homeserver returned an error";
    }
    return E;
}

ApiError MakeJsonError(long StatusCode, const char *Message) {
    ApiError E;
    E.StatusCode = StatusCode;
    E.ErrCode = "M_UNKNOWN";
    E.Message = Message;
    return E;
}

Result<nlohmann::json> DoJson(const RequestOpt &Opt) {
    const HttpResponse Resp = Execute(Opt);
    if(!Resp.TransportOk() || !Is2xx(Resp.StatusCode)) {
        return Result<nlohmann::json>::Failure(MakeError(Resp));
    }
    if(Resp.Body.empty()) {
        return Result<nlohmann::json>::Success(nlohmann::json::object());
    }
    auto Parsed = nlohmann::json::parse(Resp.Body, nullptr, false);
    if(Parsed.is_object() || Parsed.is_array()) {
        return Result<nlohmann::json>::Success(std::move(Parsed));
    }
    return Result<nlohmann::json>::Failure(MakeJsonError(Resp.StatusCode, "Invalid JSON response from the homeserver"));
}

Status DoStatus(const RequestOpt &Opt) {
    const HttpResponse Resp = Execute(Opt);
    if(!Resp.TransportOk() || !Is2xx(Resp.StatusCode)) {
        return Status::Failure(MakeError(Resp));
    }
    return Status::Success();
}

LoginResult ParseLogin(const nlohmann::json &J) {
    LoginResult Out;
    Out.AccessToken = Util::JsonGetString(J, "access_token");
    Out.RefreshToken = Util::JsonGetString(J, "refresh_token");
    Out.DeviceId = Util::JsonGetString(J, "device_id");
    Out.UserId = Util::JsonGetString(J, "user_id");
    Out.ExpiresInMs = Util::JsonGetInt(J, "expires_in_ms");
    return Out;
}

// Trailing number of a version string: "v1.11" -> 11, "r0.6.0" -> 0, "abc" -> -1.
int TrailingNumber(const std::string &Text) {
    if(Text.empty()) {
        return -1;
    }
    std::size_t End = Text.size();
    std::size_t Begin = End;
    while(Begin > 0 && Text[Begin - 1] >= '0' && Text[Begin - 1] <= '9') {
        --Begin;
    }
    if(Begin == End) {
        return -1;
    }
    int Value = 0;
    for(std::size_t I = Begin; I < End; ++I) {
        if(Value > 10000000) {
            Value = 10000000; // absurd version strings must not overflow
            break;
        }
        Value = Value * 10 + (Text[I] - '0');
    }
    return Value;
}

} // namespace

bool ServerVersions::Supports(const std::string &Version) const {
    for(const std::string &Entry : Versions) {
        if(Entry == Version) {
            return true;
        }
    }
    const int Required = TrailingNumber(Version);
    if(Required < 0) {
        return false;
    }
    for(const std::string &Entry : Versions) {
        if(Util::StartsWith(Entry, "v")) {
            const int Have = TrailingNumber(Entry);
            if(Have >= Required) {
                return true;
            }
        }
    }
    return false;
}

// --- Setup / session state ---

void Client::Configure(const std::string &BaseUrl) {
    m_BaseUrl = Util::Trim(BaseUrl);
    while(!m_BaseUrl.empty() && m_BaseUrl.back() == '/') {
        m_BaseUrl.pop_back();
    }
}

bool Client::IsConfigured() const {
    return !m_BaseUrl.empty();
}

const std::string &Client::GetBaseUrl() const {
    return m_BaseUrl;
}

void Client::SetSession(const std::string &AccessToken, const std::string &DeviceId, const std::string &UserId) {
    m_AccessToken = AccessToken;
    m_DeviceId = DeviceId;
    m_UserId = UserId;
}

void Client::ClearSession() {
    m_AccessToken.clear();
    m_DeviceId.clear();
    m_UserId.clear();
}

bool Client::HasSession() const {
    return !m_AccessToken.empty();
}

const std::string &Client::GetAccessToken() const {
    return m_AccessToken;
}

const std::string &Client::GetDeviceId() const {
    return m_DeviceId;
}

const std::string &Client::GetUserId() const {
    return m_UserId;
}

// --- Discovery / authentication ---

Result<ServerVersions> Client::GetVersions() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/versions");
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<ServerVersions>::Failure(std::move(Resp.Error));
    }
    ServerVersions Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_object() && J.contains("versions") && J["versions"].is_array()) {
        for(const auto &Entry : J["versions"]) {
            if(Entry.is_string()) {
                Out.Versions.push_back(Entry.get<std::string>());
            }
        }
    }
    if(J.is_object() && J.contains("unstable_features") && J["unstable_features"].is_object()) {
        const nlohmann::json &Features = J["unstable_features"];
        for(auto It = Features.begin(); It != Features.end(); ++It) {
            Out.UnstableFeatures[It.key()] = It.value().is_boolean() ? It.value().get<bool>() : false;
        }
    }
    return Result<ServerVersions>::Success(std::move(Out));
}

Result<nlohmann::json> Client::GetLoginFlows() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/login");
    return DoJson(Opt);
}

Result<LoginResult> Client::LoginPassword(const std::string &User, const std::string &Password) const {
    const nlohmann::json Body = {
        {"type", "m.login.password"},
        {"identifier", {{"type", "m.id.user"}, {"user", User}}},
        {"password", Password},
        {"initial_device_display_name", "NX-Matrix"},
    };
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, {}, "/_matrix/client/v3/login");
    Opt.JsonBody = &Body;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<LoginResult>::Failure(std::move(Resp.Error));
    }
    return Result<LoginResult>::Success(ParseLogin(Resp.Value));
}

Result<LoginResult> Client::Register(const std::string &User, const std::string &Password) const {
    const nlohmann::json BaseBody = {
        {"username", User},
        {"password", Password},
        {"initial_device_display_name", "NX-Matrix"},
    };
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, {}, "/_matrix/client/v3/register");
    nlohmann::json Body = BaseBody;
    std::string Session;
    std::string NextStage;
    std::size_t StageIndex = 0;
    HttpResponse Resp;
    for(int Attempt = 0; Attempt < 16; ++Attempt) {
        if(!NextStage.empty()) {
            Body["auth"] = {{"type", NextStage}, {"session", Session}};
        }
        Opt.JsonBody = &Body;
        Resp = Execute(Opt);
        if(!Resp.TransportOk()) {
            return Result<LoginResult>::Failure(MakeError(Resp));
        }
        if(Is2xx(Resp.StatusCode)) {
            const auto Parsed = nlohmann::json::parse(Resp.Body, nullptr, false);
            if(!Parsed.is_object()) {
                return Result<LoginResult>::Failure(MakeJsonError(Resp.StatusCode, "Invalid JSON response from the homeserver"));
            }
            return Result<LoginResult>::Success(ParseLogin(Parsed));
        }
        if(Resp.StatusCode != 401) {
            return Result<LoginResult>::Failure(MakeError(Resp));
        }

        // UIAA challenge: only m.login.dummy / m.login.terms can be completed here.
        const auto J = nlohmann::json::parse(Resp.Body, nullptr, false);
        if(!J.is_object() || !J.contains("session") || !J["session"].is_string() || !J.contains("flows") || !J["flows"].is_array() ||
           J["flows"].empty()) {
            return Result<LoginResult>::Failure(MakeError(Resp));
        }
        bool Supported = true;
        for(const auto &Flow : J["flows"]) {
            if(!Flow.is_object() || !Flow.contains("stages") || !Flow["stages"].is_array() || Flow["stages"].empty()) {
                Supported = false;
                break;
            }
            for(const auto &Stage : Flow["stages"]) {
                if(!Stage.is_string()) {
                    Supported = false;
                    break;
                }
                const std::string Value = Stage.get<std::string>();
                if(Value != "m.login.dummy" && Value != "m.login.terms") {
                    Supported = false;
                    break;
                }
            }
            if(!Supported) {
                break;
            }
        }
        if(!Supported) {
            ApiError E;
            E.StatusCode = Resp.StatusCode;
            E.ErrCode = "M_UNKNOWN";
            E.Message = "This homeserver requires email or captcha verification (unsupported on console)";
            return Result<LoginResult>::Failure(std::move(E));
        }

        Session = J["session"].get<std::string>();
        const nlohmann::json &Flows = J["flows"];
        std::vector<std::string> Stages;
        for(const auto &Stage : Flows[0]["stages"]) {
            Stages.push_back(Stage.get<std::string>());
        }
        if(StageIndex >= Stages.size()) {
            return Result<LoginResult>::Failure(MakeError(Resp)); // server keeps challenging past every stage
        }
        NextStage = Stages[StageIndex];
        ++StageIndex;
    }
    return Result<LoginResult>::Failure(MakeError(Resp));
}

Result<LoginResult> Client::RefreshSession(const std::string &RefreshToken) const {
    const nlohmann::json Body = {{"refresh_token", RefreshToken}};
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, {}, "/_matrix/client/v3/refresh");
    Opt.JsonBody = &Body;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<LoginResult>::Failure(std::move(Resp.Error));
    }
    return Result<LoginResult>::Success(ParseLogin(Resp.Value));
}

Status Client::Logout() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/logout");
    Opt.JsonBody = &kEmptyJson;
    return DoStatus(Opt);
}

Status Client::LogoutAll() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/logout/all");
    Opt.JsonBody = &kEmptyJson;
    return DoStatus(Opt);
}

Result<nlohmann::json> Client::WhoAmI() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/account/whoami");
    return DoJson(Opt);
}

// --- Sync / rooms / messages ---

Result<nlohmann::json> Client::Sync(const std::string &Since, int TimeoutSec) const {
    static const char *kFilter =
        R"({"room":{"timeline":{"limit":30},"state":{"lazy_load_members":true}},"presence":{"types":[]},"account_data":{"types":["m.tag"]}})";
    const long WaitMs = (TimeoutSec > 0) ? static_cast<long>(TimeoutSec) * 1000L : 0L;
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/sync");
    Opt.Query.emplace_back("filter", kFilter);
    Opt.Query.emplace_back("timeout", std::to_string(WaitMs));
    Opt.Query.emplace_back("set_presence", "online");
    if(!Since.empty()) {
        Opt.Query.emplace_back("since", Since);
    }
    Opt.TimeoutMs = WaitMs + kDefaultTimeoutMs; // long-poll must outlive the server-side wait
    return DoJson(Opt);
}

Result<std::vector<std::string>> Client::GetJoinedRooms() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/joined_rooms");
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::vector<std::string>>::Failure(std::move(Resp.Error));
    }
    std::vector<std::string> Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_array()) {
        for(const auto &Entry : J) {
            if(Entry.is_string()) {
                Out.push_back(Entry.get<std::string>());
            }
        }
    }
    return Result<std::vector<std::string>>::Success(std::move(Out));
}

Result<nlohmann::json> Client::GetRoomState(const std::string &RoomId) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/state");
    return DoJson(Opt);
}

Result<SpaceHierarchyPage> Client::GetSpaceHierarchy(const std::string &RoomId, const std::string &From, int Limit) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v1/rooms/" + Enc(RoomId) + "/hierarchy");
    if(!From.empty()) {
        Opt.Query.emplace_back("from", From);
    }
    if(Limit > 0) {
        Opt.Query.emplace_back("limit", std::to_string(Limit));
    }
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<SpaceHierarchyPage>::Failure(std::move(Resp.Error));
    }
    SpaceHierarchyPage Out;
    const nlohmann::json &J = Resp.Value;
    Out.NextBatch = Util::JsonGetString(J, "next_batch");
    if(J.contains("rooms") && J["rooms"].is_array()) {
        for(const auto &Chunk : J["rooms"]) {
            if(!Chunk.is_object()) {
                continue;
            }
            SpaceHierarchyRoom Room;
            Room.RoomId = Util::JsonGetString(Chunk, "room_id");
            if(Room.RoomId.empty()) {
                continue;
            }
            Room.Name = Util::JsonGetString(Chunk, "name");
            Room.Topic = Util::JsonGetString(Chunk, "topic");
            Room.AvatarMxc = Util::JsonGetString(Chunk, "avatar_url");
            Room.CanonicalAlias = Util::JsonGetString(Chunk, "canonical_alias");
            Room.RoomType = Util::JsonGetString(Chunk, "room_type");
            Room.JoinRule = Util::JsonGetString(Chunk, "join_rule");
            Room.NumJoinedMembers = Util::JsonGetInt(Chunk, "num_joined_members");
            Room.WorldReadable = Util::JsonGetBool(Chunk, "world_readable");
            // children_state: stripped m.space.child events; only edges with
            // a valid via count as children (spec §m.space.child).
            if(Chunk.contains("children_state") && Chunk["children_state"].is_array()) {
                for(const auto &Child : Chunk["children_state"]) {
                    if(!Child.is_object() || !Child.contains("content") || !Child["content"].is_object()) {
                        continue;
                    }
                    const auto &Content = Child["content"];
                    if(Content.contains("via") && Content["via"].is_array()) {
                        for(const auto &Via : Content["via"]) {
                            if(Via.is_string()) {
                                Room.ChildCount++;
                                break;
                            }
                        }
                    }
                }
            }
            Out.Rooms.push_back(std::move(Room));
        }
    }
    return Result<SpaceHierarchyPage>::Success(std::move(Out));
}

Result<nlohmann::json> Client::GetStateEvent(const std::string &RoomId, const std::string &EventType, const std::string &StateKey) const {
    std::string Path = "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/state/" + Enc(EventType);
    if(!StateKey.empty()) {
        Path += "/" + Enc(StateKey);
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, Path);
    return DoJson(Opt);
}

Result<BackfillResult> Client::GetMessages(const std::string &RoomId, const std::string &From, const std::string &To, bool Backward,
                                           int Limit) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/messages");
    if(!From.empty()) {
        Opt.Query.emplace_back("from", From);
    }
    Opt.Query.emplace_back("dir", Backward ? "b" : "f");
    Opt.Query.emplace_back("limit", std::to_string(Limit));
    if(!To.empty()) {
        Opt.Query.emplace_back("to", To);
    }
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<BackfillResult>::Failure(std::move(Resp.Error));
    }
    BackfillResult Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_object() && J.contains("chunk") && J["chunk"].is_array()) {
        for(const auto &Item : J["chunk"]) {
            if(Item.is_object()) {
                Out.Events.push_back(Event::FromJson(Item, RoomId));
            }
        }
    }
    if(J.is_object()) {
        Out.PrevBatch = Util::JsonGetString(J, "end");
        Out.NextBatch = Util::JsonGetString(J, "start");
    }
    return Result<BackfillResult>::Success(std::move(Out));
}

Result<std::string> Client::SendEvent(const std::string &RoomId, const std::string &EventType, const nlohmann::json &Content,
                                      const std::string &TransactionId) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken,
                             "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/send/" + Enc(EventType) + "/" + Enc(TransactionId));
    Opt.JsonBody = &Content;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::string>::Failure(std::move(Resp.Error));
    }
    return Result<std::string>::Success(Util::JsonGetString(Resp.Value, "event_id"));
}

Result<std::string> Client::SendStateEvent(const std::string &RoomId, const std::string &EventType, const std::string &StateKey,
                                           const nlohmann::json &Content) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken,
                             "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/state/" + Enc(EventType) + "/" + Enc(StateKey));
    Opt.JsonBody = &Content;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::string>::Failure(std::move(Resp.Error));
    }
    return Result<std::string>::Success(Util::JsonGetString(Resp.Value, "event_id"));
}

Status Client::RedactEvent(const std::string &RoomId, const std::string &EventId, const std::string &Reason,
                           const std::string &TransactionId) const {
    nlohmann::json Body = nlohmann::json::object();
    if(!Reason.empty()) {
        Body["reason"] = Reason;
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken,
                             "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/redact/" + Enc(EventId) + "/" + Enc(TransactionId));
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::SetTyping(const std::string &RoomId, bool Typing, int TimeoutSec) const {
    nlohmann::json Body = {
        {"typing", Typing},
        {"timeout", (TimeoutSec > 0 ? static_cast<long>(TimeoutSec) : 0L) * 1000L},
    };
    RequestOpt Opt =
        MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/typing/" + Enc(m_UserId));
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::SendReadReceipt(const std::string &RoomId, const std::string &EventId) const {
    RequestOpt Opt =
        MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/receipt/m.read/" + Enc(EventId));
    Opt.JsonBody = &kEmptyJson;
    return DoStatus(Opt);
}

Status Client::SetReadMarkers(const std::string &RoomId, const std::string &FullyReadEventId, const std::string &ReadEventId) const {
    nlohmann::json Body = nlohmann::json::object();
    if(!FullyReadEventId.empty()) {
        Body["m.fully_read"] = FullyReadEventId;
    }
    if(!ReadEventId.empty()) {
        Body["m.read"] = ReadEventId;
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/read_markers");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

// --- Room management ---

Result<std::string> Client::JoinRoom(const std::string &RoomIdOrAlias, const std::vector<std::string> &ViaServers) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/join/" + Enc(RoomIdOrAlias));
    for(const auto &Server : ViaServers) {
        Opt.Query.emplace_back("via", Server); // spec: servers to join through
    }
    Opt.JsonBody = &kEmptyJson;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::string>::Failure(std::move(Resp.Error));
    }
    return Result<std::string>::Success(Util::JsonGetString(Resp.Value, "room_id"));
}

Status Client::LeaveRoom(const std::string &RoomId, const std::string &Reason) const {
    nlohmann::json Body = nlohmann::json::object();
    if(!Reason.empty()) {
        Body["reason"] = Reason;
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/leave");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::InviteUser(const std::string &RoomId, const std::string &UserId) const {
    const nlohmann::json Body = {{"user_id", UserId}};
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/invite");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::KickUser(const std::string &RoomId, const std::string &UserId, const std::string &Reason) const {
    nlohmann::json Body = {{"user_id", UserId}};
    if(!Reason.empty()) {
        Body["reason"] = Reason;
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/kick");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::BanUser(const std::string &RoomId, const std::string &UserId, const std::string &Reason) const {
    nlohmann::json Body = {{"user_id", UserId}};
    if(!Reason.empty()) {
        Body["reason"] = Reason;
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/ban");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::UnbanUser(const std::string &RoomId, const std::string &UserId) const {
    const nlohmann::json Body = {{"user_id", UserId}};
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/unban");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::ForgetRoom(const std::string &RoomId) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/forget");
    Opt.JsonBody = &kEmptyJson;
    return DoStatus(Opt);
}

Result<std::string> Client::CreateRoom(const RoomCreateParams &Params) const {
    nlohmann::json Body = nlohmann::json::object();
    if(!Params.Name.empty()) {
        Body["name"] = Params.Name;
    }
    if(!Params.Topic.empty()) {
        Body["topic"] = Params.Topic;
    }
    if(!Params.RoomAlias.empty()) {
        Body["room_alias_name"] = Params.RoomAlias;
    }
    if(!Params.InviteUserIds.empty()) {
        Body["invite"] = Params.InviteUserIds;
    }
    if(Params.IsDirect) {
        Body["is_direct"] = true;
    }
    Body["visibility"] = Params.IsPublic ? "public" : "private";
    if(Params.IsEncrypted) {
        const nlohmann::json Encryption = {
            {"type", "m.room.encryption"},
            {"content", {{"algorithm", "m.megolm.v1.aes-sha2"}}},
        };
        Body["initial_state"] = nlohmann::json::array({Encryption});
    }
    if(Params.IsSpace) {
        Body["creation_content"] = {{"type", "m.space"}};
        // Spec v1.19 §Spaces: space-rooms should prohibit normal timeline
        // events by setting a high events_default.
        Body["power_level_content_override"] = {{"events_default", 100}};
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/createRoom");
    Opt.JsonBody = &Body;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::string>::Failure(std::move(Resp.Error));
    }
    return Result<std::string>::Success(Util::JsonGetString(Resp.Value, "room_id"));
}

Result<std::map<std::string, RoomMember>> Client::GetJoinedMembers(const std::string &RoomId) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/rooms/" + Enc(RoomId) + "/joined_members");
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::map<std::string, RoomMember>>::Failure(std::move(Resp.Error));
    }
    std::map<std::string, RoomMember> Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_object() && J.contains("joined") && J["joined"].is_object()) {
        const nlohmann::json &Joined = J["joined"];
        for(auto It = Joined.begin(); It != Joined.end(); ++It) {
            RoomMember Member;
            Member.UserId = It.key();
            if(It.value().is_object()) {
                Member.DisplayName = Util::JsonGetString(It.value(), "display_name");
                Member.AvatarMxc = Util::JsonGetString(It.value(), "avatar_url");
                Member.Membership = MembershipFromString(Util::JsonGetString(It.value(), "membership"));
            }
            Out.emplace(Member.UserId, std::move(Member));
        }
    }
    return Result<std::map<std::string, RoomMember>>::Success(std::move(Out));
}

// --- Tags / account data ---

// Spec: the tag endpoints carry the user id segment
// /user/{userId}/rooms/{roomId}/tags/{tag}; dropping it 404s with M_UNRECOGNIZED.
Status Client::SetTag(const std::string &RoomId, const std::string &Tag, double Order) const {
    const nlohmann::json Body = {{"order", Order}};
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken,
                             "/_matrix/client/v3/user/" + Enc(m_UserId) + "/rooms/" + Enc(RoomId) + "/tags/" + Enc(Tag));
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::RemoveTag(const std::string &RoomId, const std::string &Tag) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Delete, m_BaseUrl, m_AccessToken,
                             "/_matrix/client/v3/user/" + Enc(m_UserId) + "/rooms/" + Enc(RoomId) + "/tags/" + Enc(Tag));
    return DoStatus(Opt);
}

Result<nlohmann::json> Client::GetAccountData(const std::string &Type) const {
    RequestOpt Opt =
        MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/user/" + Enc(m_UserId) + "/account_data/" + Enc(Type));
    return DoJson(Opt);
}

Status Client::SetAccountData(const std::string &Type, const nlohmann::json &Content) const {
    RequestOpt Opt =
        MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/user/" + Enc(m_UserId) + "/account_data/" + Enc(Type));
    Opt.JsonBody = &Content;
    return DoStatus(Opt);
}

// --- Profile / presence ---

Result<UserProfile> Client::GetProfile(const std::string &UserId) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/profile/" + Enc(UserId));
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<UserProfile>::Failure(std::move(Resp.Error));
    }
    UserProfile Out;
    Out.UserId = UserId;
    Out.DisplayName = Util::JsonGetString(Resp.Value, "displayname");
    Out.AvatarMxc = Util::JsonGetString(Resp.Value, "avatar_url");
    return Result<UserProfile>::Success(std::move(Out));
}

Status Client::SetDisplayName(const std::string &DisplayName) const {
    const nlohmann::json Body = {{"displayname", DisplayName}};
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/profile/" + Enc(m_UserId) + "/displayname");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::SetAvatarMxc(const std::string &AvatarMxc) const {
    const nlohmann::json Body = {{"avatar_url", AvatarMxc}};
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/profile/" + Enc(m_UserId) + "/avatar_url");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Status Client::SetPresence(const std::string &Presence) const {
    const nlohmann::json Body = {{"presence", Presence}};
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/presence/" + Enc(m_UserId) + "/status");
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

// --- Media ---

Result<std::string> Client::UploadMedia(const ByteArray &Data, const std::string &ContentType, const std::string &FileName) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/media/v3/upload");
    if(!FileName.empty()) {
        Opt.Query.emplace_back("filename", FileName);
    }
    Opt.RawBody.assign(Data.begin(), Data.end());
    Opt.ContentType = ContentType.empty() ? std::string("application/octet-stream") : ContentType;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::string>::Failure(std::move(Resp.Error));
    }
    return Result<std::string>::Success(Util::JsonGetString(Resp.Value, "content_uri"));
}

Result<ByteArray> Client::DownloadMedia(const std::string &MxcUrl, int ThumbnailWidth, int ThumbnailHeight) const {
    std::string Rest = MxcUrl;
    if(Util::StartsWith(Rest, "mxc://")) {
        Rest = Rest.substr(6);
    } else {
        Rest.clear();
    }
    const std::size_t Slash = Rest.find('/');
    if(Rest.empty() || Slash == std::string::npos || Slash == 0 || Slash + 1 >= Rest.size()) {
        ApiError E;
        E.ErrCode = "M_UNKNOWN";
        E.Message = "Invalid mxc:// URL";
        return Result<ByteArray>::Failure(std::move(E));
    }
    const std::string ServerName = Rest.substr(0, Slash);
    const std::string MediaId = Rest.substr(Slash + 1);

    const bool Thumbnail = ThumbnailWidth > 0 || ThumbnailHeight > 0;
    const std::string Kind = Thumbnail ? "thumbnail" : "download";
    RequestOpt Opt =
        MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v1/media/" + Kind + "/" + Enc(ServerName) + "/" + Enc(MediaId));
    if(Thumbnail) {
        if(ThumbnailWidth > 0) {
            Opt.Query.emplace_back("width", std::to_string(ThumbnailWidth));
        }
        if(ThumbnailHeight > 0) {
            Opt.Query.emplace_back("height", std::to_string(ThumbnailHeight));
        }
        Opt.Query.emplace_back("method", "scale");
    }
    HttpResponse Resp = Execute(Opt);
    if(Resp.TransportOk() && (Resp.StatusCode == 404 || Resp.StatusCode == 400)) {
        Opt.Path = "/_matrix/media/v3/" + Kind + "/" + Enc(ServerName) + "/" + Enc(MediaId);
        Resp = Execute(Opt);
    }
    if(!Resp.TransportOk() || !Is2xx(Resp.StatusCode)) {
        return Result<ByteArray>::Failure(MakeError(Resp));
    }
    return Result<ByteArray>::Success(ByteArray(Resp.Body.begin(), Resp.Body.end()));
}

// --- Search / devices ---

Result<SearchResult> Client::SearchMessages(const std::string &Term, int Limit) const {
    // nlohmann builds an array unless every element of the outer list is a
    // braced {key, value} pair, so each nesting level needs doubled braces:
    // search_categories must be an object mapping category -> criteria (an
    // array makes the server fail the request), and the result cap lives in
    // filter.limit, not on the criteria itself (spec v1.19, /search).
    const nlohmann::json Criteria = {
        {"search_term", Term},
        {"order_by", "recent"},
        {"event_context", {{"before_limit", 10}, {"after_limit", 10}}},
        {"filter", {{"limit", Limit}}},
    };
    const nlohmann::json Body = {{"search_categories", {{"room_events", Criteria}}}};
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/search");
    Opt.JsonBody = &Body;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<SearchResult>::Failure(std::move(Resp.Error));
    }
    SearchResult Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_object() && J.contains("search_categories") && J["search_categories"].is_object()) {
        const nlohmann::json &Categories = J["search_categories"];
        if(Categories.contains("room_events") && Categories["room_events"].is_object()) {
            const nlohmann::json &RoomEvents = Categories["room_events"];
            if(RoomEvents.contains("results") && RoomEvents["results"].is_array()) {
                for(const auto &Item : RoomEvents["results"]) {
                    if(!Item.is_object()) {
                        continue;
                    }
                    if(Item.contains("results") && Item["results"].is_array()) {
                        for(const auto &Nested : Item["results"]) {
                            if(Nested.is_object() && Nested.contains("result") && Nested["result"].is_object()) {
                                const nlohmann::json &Ev = Nested["result"];
                                Out.Events.push_back(Event::FromJson(Ev, Util::JsonGetString(Ev, "room_id")));
                            }
                        }
                    } else if(Item.contains("result") && Item["result"].is_object()) {
                        const nlohmann::json &Ev = Item["result"];
                        Out.Events.push_back(Event::FromJson(Ev, Util::JsonGetString(Ev, "room_id")));
                    }
                }
            }
        }
    }
    Out.ResultCount = Out.Events.size();
    return Result<SearchResult>::Success(std::move(Out));
}

Result<std::vector<DeviceInfo>> Client::GetDevices() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/devices");
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::vector<DeviceInfo>>::Failure(std::move(Resp.Error));
    }
    std::vector<DeviceInfo> Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_object() && J.contains("devices") && J["devices"].is_array()) {
        for(const auto &Item : J["devices"]) {
            if(!Item.is_object()) {
                continue;
            }
            DeviceInfo Device;
            Device.DeviceId = Util::JsonGetString(Item, "device_id");
            Device.DisplayName = Util::JsonGetString(Item, "display_name");
            Device.LastSeenIp = Util::JsonGetString(Item, "last_seen_ip");
            Device.LastSeenTs = Util::JsonGetInt(Item, "last_seen_ts");
            Out.push_back(std::move(Device));
        }
    }
    return Result<std::vector<DeviceInfo>>::Success(std::move(Out));
}

Status Client::DeleteDevice(const std::string &DeviceId, const std::string &Password) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Delete, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/devices/" + Enc(DeviceId));
    HttpResponse Resp = Execute(Opt);
    if(Resp.TransportOk() && Resp.StatusCode == 401) {
        const auto First = nlohmann::json::parse(Resp.Body, nullptr, false);
        std::string Session;
        if(First.is_object()) {
            Session = Util::JsonGetString(First, "session");
        }
        if(!Session.empty()) {
            const nlohmann::json Body = {
                {"auth",
                 {
                     {"type", "m.login.password"},
                     {"identifier", {{"type", "m.id.user"}, {"user", m_UserId}}},
                     {"password", Password},
                     {"session", Session},
                 }},
            };
            Opt.JsonBody = &Body;
            Resp = Execute(Opt);
        }
    }
    if(!Resp.TransportOk() || !Is2xx(Resp.StatusCode)) {
        return Status::Failure(MakeError(Resp));
    }
    return Status::Success();
}

// --- E2EE endpoints ---

Result<nlohmann::json> Client::UploadKeys(const nlohmann::json &KeysBody) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/keys/upload");
    Opt.JsonBody = &KeysBody;
    return DoJson(Opt);
}

Result<nlohmann::json> Client::QueryKeys(const nlohmann::json &QueryBody) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/keys/query");
    Opt.JsonBody = &QueryBody;
    return DoJson(Opt);
}

Result<nlohmann::json> Client::ClaimKeys(const nlohmann::json &ClaimBody) const {
    nlohmann::json Body = ClaimBody;
    if(!Body.is_object()) {
        Body = nlohmann::json::object();
    }
    Body["timeout"] = 5000; // spec: milliseconds to wait for keys from remote servers
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/keys/claim");
    Opt.JsonBody = &Body;
    return DoJson(Opt);
}

Status Client::SendToDevice(const std::string &EventType, const nlohmann::json &Messages, const std::string &TransactionId) const {
    const nlohmann::json Body = {{"messages", Messages}};
    RequestOpt Opt =
        MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/sendToDevice/" + Enc(EventType) + "/" + Enc(TransactionId));
    Opt.JsonBody = &Body;
    return DoStatus(Opt);
}

Result<std::vector<std::string>> Client::GetKeyChanges(const std::string &FromToken, const std::string &ToToken) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/keys/changes");
    Opt.Query.emplace_back("from", FromToken);
    Opt.Query.emplace_back("to", ToToken);
    auto Resp = DoJson(Opt);
    if(!Resp.Ok) {
        return Result<std::vector<std::string>>::Failure(std::move(Resp.Error));
    }
    std::vector<std::string> Out;
    const nlohmann::json &J = Resp.Value;
    if(J.is_object() && J.contains("changed") && J["changed"].is_array()) {
        for(const auto &Entry : J["changed"]) {
            if(Entry.is_string()) {
                Out.push_back(Entry.get<std::string>());
            }
        }
    }
    return Result<std::vector<std::string>>::Success(std::move(Out));
}

namespace {

// /room_keys paths are v1 in the spec; some deployments only expose aliases.
// Retry once on 404 with the other stable version.
template <typename T> Result<T> WithRoomKeysFallback(RequestOpt Opt, Result<T> Result0) {
    if(Result0.Ok || Result0.Error.StatusCode != 404) {
        return Result0;
    }
    Opt.Path = Util::ReplaceAll(Opt.Path, "/client/v1/", "/client/v3/");
    if constexpr(std::is_same_v<T, nlohmann::json>) {
        return DoJson(Opt);
    } else {
        const Status St = DoStatus(Opt);
        if(!St.Ok) {
            return Result<T>::Failure(St.Error);
        }
        return Result<T>::Success(T{});
    }
}

} // namespace

Result<nlohmann::json> Client::GetRoomKeysLatestVersion() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v1/room_keys/version");
    return DoJson(Opt);
}

Result<std::vector<std::string>> Client::GetRoomKeysVersions() const {
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, "/_matrix/client/v1/room_keys/versions");
    auto Resp = DoJson(Opt);
    if(!Resp.Ok && Resp.Error.StatusCode == 404) {
        Opt.Path = Util::ReplaceAll(Opt.Path, "/client/v1/", "/client/v3/");
        Resp = DoJson(Opt);
    }
    if(!Resp.Ok) {
        return Result<std::vector<std::string>>::Failure(std::move(Resp.Error));
    }
    std::vector<std::string> Out;
    if(Resp.Value.contains("versions") && Resp.Value["versions"].is_array()) {
        for(const auto &Entry : Resp.Value["versions"]) {
            if(Entry.is_string()) {
                Out.push_back(Entry.get<std::string>());
            }
        }
    }
    return Result<std::vector<std::string>>::Success(std::move(Out));
}

Result<std::string> Client::CreateRoomKeysVersion(const nlohmann::json &VersionInfo) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v1/room_keys/version");
    Opt.JsonBody = &VersionInfo;
    auto Resp = DoJson(Opt);
    if(!Resp.Ok && Resp.Error.StatusCode == 404) {
        Opt.Path = Util::ReplaceAll(Opt.Path, "/client/v1/", "/client/v3/");
        Resp = DoJson(Opt);
    }
    if(!Resp.Ok) {
        return Result<std::string>::Failure(std::move(Resp.Error));
    }
    return Result<std::string>::Success(Util::JsonGetString(Resp.Value, "version"));
}

Result<nlohmann::json> Client::GetRoomKeysSessions(const std::string &Version, const std::string &RoomId,
                                                   const std::string &SessionId) const {
    std::string Path = "/_matrix/client/v1/room_keys/keys";
    if(!RoomId.empty()) {
        Path += "/" + Enc(RoomId);
        if(!SessionId.empty()) {
            Path += "/" + Enc(SessionId);
        }
    }
    RequestOpt Opt = MakeOpt(HttpMethod::Get, m_BaseUrl, m_AccessToken, Path);
    if(!Version.empty()) {
        Opt.Query.emplace_back("version", Version);
    }
    return DoJson(Opt);
}

Status Client::UploadRoomKeys(const std::string &Version, const nlohmann::json &Sessions) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Put, m_BaseUrl, m_AccessToken, "/_matrix/client/v1/room_keys/keys");
    if(!Version.empty()) {
        Opt.Query.emplace_back("version", Version);
    }
    Opt.JsonBody = &Sessions;
    HttpResponse Resp = Execute(Opt);
    if(Resp.TransportOk() && Resp.StatusCode == 404) {
        Opt.Path = Util::ReplaceAll(Opt.Path, "/client/v1/", "/client/v3/");
        Resp = Execute(Opt);
    }
    if(!Resp.TransportOk() || !Is2xx(Resp.StatusCode)) {
        return Status::Failure(MakeError(Resp));
    }
    return Status::Success();
}

Result<nlohmann::json> Client::UploadDeviceSigning(const nlohmann::json &SigningKeys, const std::string &Password) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/keys/device_signing/upload");
    Opt.JsonBody = &SigningKeys;
    HttpResponse Resp = Execute(Opt);
    if(Resp.TransportOk() && Resp.StatusCode == 401 && !Password.empty()) {
        // UIAA: stage a password auth dict and retry once.
        const auto First = nlohmann::json::parse(Resp.Body, nullptr, false);
        std::string Session;
        if(First.is_object()) {
            Session = Util::JsonGetString(First, "session");
        }
        if(!Session.empty()) {
            nlohmann::json Body = SigningKeys;
            Body["auth"] = {
                {"type", "m.login.password"},
                {"identifier", {{"type", "m.id.user"}, {"user", m_UserId}}},
                {"password", Password},
                {"session", Session},
            };
            Opt.JsonBody = &Body;
            Resp = Execute(Opt);
        }
    }
    if(!Resp.TransportOk() || !Is2xx(Resp.StatusCode)) {
        return Result<nlohmann::json>::Failure(MakeError(Resp));
    }
    if(Resp.Body.empty()) {
        return Result<nlohmann::json>::Success(nlohmann::json::object());
    }
    auto Parsed = nlohmann::json::parse(Resp.Body, nullptr, false);
    if(!Parsed.is_object() && !Parsed.is_array()) {
        return Result<nlohmann::json>::Success(nlohmann::json::object());
    }
    return Result<nlohmann::json>::Success(std::move(Parsed));
}

Result<nlohmann::json> Client::UploadSignatures(const nlohmann::json &Signatures) const {
    RequestOpt Opt = MakeOpt(HttpMethod::Post, m_BaseUrl, m_AccessToken, "/_matrix/client/v3/keys/signatures/upload");
    Opt.JsonBody = &Signatures;
    return DoJson(Opt);
}

} // namespace NxMatrix::Matrix