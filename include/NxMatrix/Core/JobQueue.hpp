#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace NxMatrix::Core {

// Small fixed thread pool for blocking work (HTTP, disk, crypto).
// Jobs must not touch Plutonium directly: marshal UI work through Dispatcher.
class JobQueue {
public:
    static JobQueue &Instance();

    void Start(std::size_t WorkerCount = 3);
    // Runs the remaining queued jobs, then joins the workers.
    void Stop();
    void Enqueue(std::function<void()> Job);

private:
    JobQueue() = default;

    std::mutex m_Mutex;
    std::condition_variable m_Ready;
    std::deque<std::function<void()>> m_Jobs;
    std::vector<std::thread> m_Workers;
    bool m_Running = false;
};

} // namespace NxMatrix::Core