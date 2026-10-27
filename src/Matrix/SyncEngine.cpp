// Blocking /sync long-poll loop with exponential backoff, marshalling its
// callbacks to the UI thread through Core::Dispatcher.
#include "NxMatrix/Matrix/SyncEngine.hpp"

#include "NxMatrix/Core/Dispatcher.hpp"
#include "NxMatrix/E2ee/E2eeManager.hpp"
#include "NxMatrix/Matrix/Client.hpp"
#include "NxMatrix/Matrix/Session.hpp"
#include "NxMatrix/Util/Json.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace NxMatrix::Matrix {

namespace {

constexpr std::uint32_t SyncTimeoutSec = 30;
constexpr std::uint32_t BackoffCapSec = 30;

std::vector<nlohmann::json> ReadJsonArray(const nlohmann::json &Parent, const std::string &Key) {
    std::vector<nlohmann::json> Out;
    if(!Parent.contains(Key) || !Parent[Key].is_array()) {
        return Out;
    }
    for(const auto &Item : Parent[Key]) {
        if(Item.is_object()) {
            Out.push_back(Item);
        }
    }
    return Out;
}

std::vector<std::string> ReadStringArray(const nlohmann::json &Parent, const std::string &Key) {
    std::vector<std::string> Out;
    if(!Parent.contains(Key) || !Parent[Key].is_array()) {
        return Out;
    }
    for(const auto &Item : Parent[Key]) {
        if(Item.is_string()) {
            Out.push_back(Item.get<std::string>());
        }
    }
    return Out;
}

} // namespace

SyncEngine::SyncEngine(Client &Api, Session &State, E2ee::E2eeManager &E2ee) : m_Api(Api), m_State(State), m_E2ee(E2ee) {}

SyncEngine::~SyncEngine() {
    Stop();
}

void SyncEngine::Start(const std::string &InitialSinceToken) {
    if(m_Thread.joinable()) {
        // Previous run ended by itself (Expired): reap before restarting.
        m_Thread.join();
    }
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        m_SinceToken = InitialSinceToken;
        m_Kicked = false;
    }
    m_Running.store(true);
    SetState(ConnectionState::Connecting);
    m_Thread = std::thread(&SyncEngine::RunLoop, this);
}

void SyncEngine::Stop() {
    m_Running.store(false);
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        m_Kicked = false;
    }
    m_Ready.notify_all();
    if(m_Thread.joinable()) {
        m_Thread.join();
    }
}

bool SyncEngine::IsRunning() const {
    return m_Running.load();
}

SyncEngine::ConnectionState SyncEngine::GetConnectionState() const {
    return m_StateCode.load();
}

std::string SyncEngine::GetSinceToken() const {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    return m_SinceToken;
}

void SyncEngine::SetOnStateChanged(std::function<void(ConnectionState)> Callback) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_OnStateChanged = std::move(Callback);
}

void SyncEngine::SetOnSyncApplied(std::function<void(const SyncSummary &)> Callback) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_OnSyncApplied = std::move(Callback);
}

void SyncEngine::Kick() {
    // Cannot abort the curl long-poll: when the loop is idle inside
    // Client::Sync it returns within SyncTimeoutSec on its own. The flag only
    // cuts the backoff wait short so the next attempt starts immediately.
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        m_Kicked = true;
    }
    m_Ready.notify_all();
}

void SyncEngine::SetState(ConnectionState State) {
    m_StateCode.store(State);
    std::function<void(ConnectionState)> Callback;
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        Callback = m_OnStateChanged;
    }
    if(Callback) {
        Core::Dispatcher::Instance().Post([Callback, State]() { Callback(State); });
    }
}

void SyncEngine::RunLoop() {
    std::uint32_t BackoffSec = 1;
    while(m_Running.load()) {
        SetState(ConnectionState::Connecting);

        std::string Since;
        {
            std::lock_guard<std::mutex> Lock(m_Mutex);
            Since = m_SinceToken;
        }

        Result<nlohmann::json> Sync = m_Api.Sync(Since, SyncTimeoutSec);
        if(!m_Running.load()) {
            break; // Stop() during the poll: drop the late response
        }

        if(!Sync.Ok) {
            if(Sync.Error.ErrCode == "M_UNKNOWN_TOKEN" || Sync.Error.ErrCode == "M_MISSING_TOKEN") {
                SetState(ConnectionState::Expired);
                m_Running.store(false);
                break;
            }
            // Transport failures and transient server errors both back off.
            SetState(ConnectionState::Offline);
            {
                std::unique_lock<std::mutex> Lock(m_Mutex);
                m_Ready.wait_for(Lock, std::chrono::seconds(BackoffSec), [this] { return !m_Running.load() || m_Kicked; });
            }
            if(!m_Running.load()) {
                break;
            }
            BackoffSec = std::min(BackoffSec * 2, BackoffCapSec);
            {
                std::lock_guard<std::mutex> Lock(m_Mutex);
                if(m_Kicked) {
                    m_Kicked = false;
                    BackoffSec = 1; // manual refresh: next failure waits fresh
                }
            }
            continue;
        }

        const nlohmann::json &Body = Sync.Value;

        // Room keys may arrive in this very batch, so feed E2ee BEFORE the
        // timeline of the same response is decrypted.
        std::vector<nlohmann::json> ToDevice;
        if(Body.contains("to_device") && Body["to_device"].is_object()) {
            ToDevice = ReadJsonArray(Body["to_device"], "events");
        }
        std::vector<std::string> ChangedDevices;
        if(Body.contains("device_lists") && Body["device_lists"].is_object()) {
            ChangedDevices = ReadStringArray(Body["device_lists"], "changed");
        }
        std::size_t OneTimeKeyCount = 0;
        if(Body.contains("device_one_time_keys_count") && Body["device_one_time_keys_count"].is_object()) {
            const auto &Keys = Body["device_one_time_keys_count"];
            if(Keys.contains("signed_curve25519") && Keys["signed_curve25519"].is_number()) {
                OneTimeKeyCount = static_cast<std::size_t>(Util::JsonGetInt(Keys, "signed_curve25519"));
            }
        }
        m_E2ee.OnSync(ToDevice, ChangedDevices, OneTimeKeyCount);

        SyncSummary Summary;
        m_State.ApplySync(Body, Summary);

        const std::string NextBatch = Util::JsonGetString(Body, "next_batch");
        {
            std::lock_guard<std::mutex> Lock(m_Mutex);
            if(!NextBatch.empty()) {
                m_SinceToken = NextBatch;
            }
        }

        SetState(ConnectionState::Online);
        BackoffSec = 1;

        std::function<void(const SyncSummary &)> OnSyncApplied;
        {
            std::lock_guard<std::mutex> Lock(m_Mutex);
            OnSyncApplied = m_OnSyncApplied;
        }
        if(OnSyncApplied) {
            Core::Dispatcher::Instance().Post([OnSyncApplied, Summary]() { OnSyncApplied(Summary); });
        }
    }
}

} // namespace NxMatrix::Matrix