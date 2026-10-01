#include "Engine/Core/JobSystem.h"

#include <algorithm>

namespace px::jobs {

namespace detail {

void JobStateBase::Execute() {
    {
        std::scoped_lock lock(m_mutex);
        if (m_status != Status::Queued) return;
        m_status = Status::Running;
    }
    Run(m_stop.get_token());
    {
        std::scoped_lock lock(m_mutex);
        m_status = Status::Done;
    }
    m_changed.notify_all();
}

bool JobStateBase::Cancel() {
    std::unique_lock lock(m_mutex);
    switch (m_status) {
    case Status::Queued:
        m_status = Status::Cancelled;
        lock.unlock();
        m_changed.notify_all();
        return true;
    case Status::Running:
        m_stop.request_stop();
        return false;
    case Status::Done:
        return false;
    case Status::Cancelled:
        return true;
    }
    return false;
}

void JobStateBase::Wait() const {
    std::unique_lock lock(m_mutex);
    m_changed.wait(lock, [this] { return Finished(); });
}

}  // namespace detail

std::size_t JobSystem::DefaultWorkerCount() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return 0;
#else
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t spare = hardware > 1 ? hardware - 1 : 2;
    return std::clamp<std::size_t>(spare, 2, 8);
#endif
}

JobSystem::JobSystem(const std::optional<std::size_t> workers) {
    const std::size_t count = workers.value_or(DefaultWorkerCount());
    m_workers.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        m_workers.emplace_back([this] { WorkerLoop(); });
}

JobSystem::~JobSystem() {
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_available.notify_all();
    // Workers finish every task already queued, so no handle waits forever.
    for (auto& worker : m_workers) worker.join();
}

JobSystem& JobSystem::Shared() {
    static JobSystem system;
    return system;
}

std::size_t JobSystem::QueuedCount() const {
    std::scoped_lock lock(m_mutex);
    return m_queue.size();
}

void JobSystem::Enqueue(std::function<void()> task) {
    if (m_workers.empty()) {
        task();
        return;
    }
    {
        std::scoped_lock lock(m_mutex);
        m_queue.push_back(std::move(task));
    }
    m_available.notify_one();
}

void JobSystem::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_available.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

void JobSystem::Strand::Push(std::function<void()> task) {
    bool schedule = false;
    {
        std::scoped_lock lock(m_queue->mutex);
        m_queue->pending.push_back(std::move(task));
        if (!m_queue->scheduled) {
            m_queue->scheduled = true;
            schedule = true;
        }
    }
    if (schedule)
        m_system->Enqueue([system = m_system, queue = m_queue] {
            Drain(system, queue);
        });
}

void JobSystem::Strand::Drain(JobSystem* system, std::shared_ptr<Queue> queue) {
    std::function<void()> task;
    {
        std::scoped_lock lock(queue->mutex);
        task = std::move(queue->pending.front());
        queue->pending.pop_front();
    }
    task();
    bool more = false;
    {
        std::scoped_lock lock(queue->mutex);
        more = !queue->pending.empty();
        if (!more) queue->scheduled = false;
    }
    // Re-queue instead of looping so one strand cannot monopolize a worker.
    if (more)
        system->Enqueue([system, queue = std::move(queue)] { Drain(system, queue); });
}

}  // namespace px::jobs
