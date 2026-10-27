#include "NxMatrix/Core/JobQueue.hpp"

namespace NxMatrix::Core {

JobQueue &JobQueue::Instance() {
    static JobQueue Instance;
    return Instance;
}

void JobQueue::Start(std::size_t WorkerCount) {
    std::lock_guard<std::mutex> Lock(m_Mutex);
    if(m_Running) {
        return;
    }
    m_Running = true;
    for(std::size_t I = 0; I < WorkerCount; ++I) {
        m_Workers.emplace_back([this]() {
            while(true) {
                std::function<void()> Job;
                {
                    std::unique_lock<std::mutex> Lock(m_Mutex);
                    m_Ready.wait(Lock, [this]() { return !m_Running || !m_Jobs.empty(); });
                    if(m_Jobs.empty()) {
                        return; // stopped and fully drained
                    }
                    Job = std::move(m_Jobs.front());
                    m_Jobs.pop_front();
                }
                Job();
            }
        });
    }
}

void JobQueue::Stop() {
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        if(!m_Running && m_Workers.empty()) {
            return; // never started or already stopped
        }
        m_Running = false;
    }
    m_Ready.notify_all();
    // Workers finish the queued jobs before noticing m_Running == false.
    for(std::thread &Worker : m_Workers) {
        if(Worker.joinable()) {
            Worker.join();
        }
    }
    m_Workers.clear();
}

void JobQueue::Enqueue(std::function<void()> Job) {
    if(!Job) {
        return;
    }
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        m_Jobs.push_back(std::move(Job));
    }
    m_Ready.notify_one();
}

} // namespace NxMatrix::Core