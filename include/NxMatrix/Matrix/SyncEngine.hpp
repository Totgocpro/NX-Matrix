#pragma once
#include "NxMatrix/Matrix/Models.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace NxMatrix::E2ee {
class E2eeManager;
}

namespace NxMatrix::Matrix {

class Client;
class Session;

// Long-polling /sync loop on a dedicated thread.
// - keeps the since token in memory (persisted by the app layer)
// - exponential backoff on transport errors (1s -> 30s cap)
// - M_UNKNOWN_TOKEN => ConnectionState::Expired (app must re-login)
// - callbacks are marshalled to the UI thread through Core::Dispatcher
class SyncEngine {
public:
    enum class ConnectionState {
        Stopped,
        Connecting,
        Online,
        Offline,
        Expired,
    };

    SyncEngine(Client &Api, Session &State, E2ee::E2eeManager &E2ee);
    ~SyncEngine();

    void Start(const std::string &InitialSinceToken);
    void Stop();
    bool IsRunning() const;

    ConnectionState GetConnectionState() const;
    std::string GetSinceToken() const;
    // True once a /sync batch was applied since Start(). The room list uses it
    // to tell "no rooms yet" apart from "first fetch still running".
    bool HasCompletedInitialSync() const;

    void SetOnStateChanged(std::function<void(ConnectionState)> Callback);
    void SetOnSyncApplied(std::function<void(const SyncSummary &)> Callback);

    // Wakes the loop immediately (after login or a manual refresh).
    void Kick();

private:
    void RunLoop();
    void SetState(ConnectionState State);

    Client &m_Api;
    Session &m_State;
    E2ee::E2eeManager &m_E2ee;

    std::thread m_Thread;
    mutable std::mutex m_Mutex;
    std::condition_variable m_Ready;
    std::string m_SinceToken;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_HasCompletedInitialSync{false};
    std::atomic<ConnectionState> m_StateCode{ConnectionState::Stopped};
    std::function<void(ConnectionState)> m_OnStateChanged;
    std::function<void(const SyncSummary &)> m_OnSyncApplied;
    // Set by Kick(), consumed by the backoff wait. Cannot abort an in-flight
    // curl long-poll, so a Kick during Sync() only takes effect afterwards.
    bool m_Kicked = false;
};

} // namespace NxMatrix::Matrix