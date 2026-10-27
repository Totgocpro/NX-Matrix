#include "NxMatrix/Core/Dispatcher.hpp"

namespace NxMatrix::Core {

Dispatcher &Dispatcher::Instance() {
    static Dispatcher Instance;
    return Instance;
}

void Dispatcher::Post(std::function<void()> Task) {
    if(!Task) {
        return;
    }
    std::lock_guard<std::mutex> Lock(m_Mutex);
    m_Tasks.push_back(std::move(Task));
}

void Dispatcher::Drain() {
    // Swap out first: tasks posted while we run land in the fresh vector and
    // execute next frame instead of growing the current loop.
    std::vector<std::function<void()>> Tasks;
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        Tasks.swap(m_Tasks);
    }
    for(std::function<void()> &Task : Tasks) {
        Task();
    }
}

} // namespace NxMatrix::Core