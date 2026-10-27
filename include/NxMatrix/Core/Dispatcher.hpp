#pragma once
#include <functional>
#include <mutex>
#include <vector>

namespace NxMatrix::Core {

// Marshals work from worker threads onto the UI (render) thread.
// Post() is thread-safe; Drain() runs the pending tasks and must only be
// called from the thread driving the UI.
class Dispatcher {
public:
    static Dispatcher &Instance();

    void Post(std::function<void()> Task);
    void Drain();

private:
    Dispatcher() = default;

    std::mutex m_Mutex;
    std::vector<std::function<void()>> m_Tasks;
};

} // namespace NxMatrix::Core