#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace px::jobs {

// Thrown by Job::get() when the job was cancelled before it started.
class JobCancelled final : public std::runtime_error {
public:
    JobCancelled() : std::runtime_error("job was cancelled before it started") {}
};

namespace detail {

class JobStateBase {
public:
    enum class Status : std::uint8_t { Queued, Running, Done, Cancelled };
    virtual ~JobStateBase() = default;

    // Runs the job unless it already started or was cancelled.
    void Execute();
    // Called by a thread about to wait for the result: runs a job that no
    // worker has started yet right here, so the waiter never queues behind
    // unrelated work. Strand jobs opt out to keep their order.
    void RunHereIfQueued() {
        if (m_runOnWait) Execute();
    }
    void SetRunOnWait(const bool enabled) { m_runOnWait = enabled; }
    // True when the job will never run. A running job is asked to stop
    // through its std::stop_token.
    bool Cancel();
    void Wait() const;
    template <typename Rep, typename Period>
    [[nodiscard]] bool WaitFor(const std::chrono::duration<Rep, Period> timeout) const {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [this] { return Finished(); });
    }
    [[nodiscard]] Status CurrentStatus() const {
        std::scoped_lock lock(m_mutex);
        return m_status;
    }

protected:
    virtual void Run(std::stop_token stop) = 0;
    [[nodiscard]] bool Finished() const {
        return m_status == Status::Done || m_status == Status::Cancelled;
    }

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    Status m_status = Status::Queued;
    bool m_runOnWait = true;
    std::stop_source m_stop;
    std::exception_ptr m_error;
};

template <typename T>
class JobState : public JobStateBase {
public:
    T Take() {
        RunHereIfQueued();
        Wait();
        std::scoped_lock lock(m_mutex);
        if (m_status == Status::Cancelled) throw JobCancelled{};
        if (m_error) std::rethrow_exception(m_error);
        if constexpr (!std::is_void_v<T>) return std::move(*m_value);
    }

protected:
    template <typename Fn>
    void Invoke(Fn& function, std::stop_token stop) {
        try {
            if constexpr (std::is_void_v<T>) {
                function(std::move(stop));
            } else {
                auto value = function(std::move(stop));
                std::scoped_lock lock(m_mutex);
                m_value.emplace(std::move(value));
            }
        } catch (...) {
            std::scoped_lock lock(m_mutex);
            m_error = std::current_exception();
        }
    }

private:
    std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> m_value;
};

// Holds the callable itself, so move-only captures work.
template <typename T, typename Fn>
class FunctionJobState final : public JobState<T> {
public:
    explicit FunctionJobState(Fn function) : m_function(std::move(function)) {}

private:
    void Run(std::stop_token stop) override {
        this->Invoke(m_function, std::move(stop));
    }
    Fn m_function;
};

}  // namespace detail

// Handle to one submitted job; move-only, like std::future. Unlike a
// std::async future, dropping a handle whose job has not started cancels
// it instead of running it. Dropping a handle whose job is running waits
// for it, because the job may use objects its owner is about to destroy.
// When a frame must not wait, Cancel() and keep the handle on a
// retirement list until it is Ready().
template <typename T>
class Job {
public:
    Job() = default;
    explicit Job(std::shared_ptr<detail::JobState<T>> state)
        : m_state(std::move(state)) {}
    Job(Job&&) noexcept = default;
    Job& operator=(Job&& other) noexcept {
        if (this != &other) {
            Release();
            m_state = std::move(other.m_state);
        }
        return *this;
    }
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() { Release(); }

    [[nodiscard]] bool valid() const { return m_state != nullptr; }
    // Waits for the job; a job no worker has started yet runs on this thread.
    void wait() const {
        if (!m_state) return;
        m_state->RunHereIfQueued();
        m_state->Wait();
    }
    template <typename Rep, typename Period>
    [[nodiscard]] std::future_status wait_for(
        const std::chrono::duration<Rep, Period> timeout) const {
        return m_state && m_state->WaitFor(timeout) ? std::future_status::ready
                                                   : std::future_status::timeout;
    }
    // Finished: done, failed or cancelled.
    [[nodiscard]] bool Ready() const {
        return wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    // The result, the job's exception, or JobCancelled. Waits if needed (a
    // job no worker has started runs on this thread) and invalidates the
    // handle, like std::future::get().
    T get() {
        auto state = std::move(m_state);
        return state->Take();
    }
    // True when the job will never run; a running job is asked to stop.
    bool Cancel() { return m_state && m_state->Cancel(); }
    [[nodiscard]] bool Cancelled() const {
        return m_state && m_state->CurrentStatus() ==
                              detail::JobStateBase::Status::Cancelled;
    }

private:
    void Release() {
        if (!m_state) return;
        if (!m_state->Cancel()) m_state->Wait();
        m_state.reset();
    }

    std::shared_ptr<detail::JobState<T>> m_state;
};

// A fixed-size worker pool. It replaces one-thread-per-call std::async,
// so bursts (skipping through voiced lines, a route full of images) queue
// up instead of creating unbounded threads.
class JobSystem {
    template <typename F>
    static auto MakeState(F&& function) {
        using Fn = std::decay_t<F>;
        if constexpr (std::is_invocable_v<Fn&, std::stop_token>) {
            using T = std::invoke_result_t<Fn&, std::stop_token>;
            return std::shared_ptr<detail::JobState<T>>(
                std::make_shared<detail::FunctionJobState<T, Fn>>(
                    Fn(std::forward<F>(function))));
        } else {
            using T = std::invoke_result_t<Fn&>;
            auto adapted = [fn = Fn(std::forward<F>(function))](
                               std::stop_token) mutable { return fn(); };
            return std::shared_ptr<detail::JobState<T>>(
                std::make_shared<detail::FunctionJobState<T, decltype(adapted)>>(
                    std::move(adapted)));
        }
    }
    template <typename S>
    struct ResultOf;
    template <typename T>
    struct ResultOf<std::shared_ptr<detail::JobState<T>>> {
        using type = T;
    };

public:
    // Workers default to DefaultWorkerCount(). Zero workers runs every job
    // inline on the submitting thread, which is how targets without threads
    // (the WASM preview) behave.
    explicit JobSystem(std::optional<std::size_t> workers = std::nullopt);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // The process-wide pool the engine uses.
    [[nodiscard]] static JobSystem& Shared();
    // hardware_concurrency - 1, clamped to [2, 8]; 0 without threads.
    [[nodiscard]] static std::size_t DefaultWorkerCount();

    // `function` takes no arguments, or a std::stop_token for cooperative
    // cancellation. Jobs start in submission order.
    template <typename F>
    [[nodiscard]] auto Submit(F&& function) {
        auto state = MakeState(std::forward<F>(function));
        using T = typename ResultOf<decltype(state)>::type;
        Enqueue([state] { state->Execute(); });
        return Job<T>(std::move(state));
    }

    [[nodiscard]] std::size_t WorkerCount() const { return m_workers.size(); }
    // Tasks waiting for a worker (a strand counts once while it has work).
    [[nodiscard]] std::size_t QueuedCount() const;

    // Jobs submitted to one strand run one at a time, in submission order,
    // on the pool's workers. Use it for work that must neither overlap nor
    // reorder, such as writes into the same save directory.
    class Strand {
    public:
        explicit Strand(JobSystem& system = JobSystem::Shared())
            : m_system(&system) {}
        Strand(const Strand&) = delete;
        Strand& operator=(const Strand&) = delete;

        template <typename F>
        [[nodiscard]] auto Submit(F&& function) {
            auto state = MakeState(std::forward<F>(function));
            using T = typename ResultOf<decltype(state)>::type;
            state->SetRunOnWait(false);
            Push([state] { state->Execute(); });
            return Job<T>(std::move(state));
        }

    private:
        struct Queue {
            std::mutex mutex;
            std::deque<std::function<void()>> pending;
            bool scheduled = false;
        };
        void Push(std::function<void()> task);
        static void Drain(JobSystem* system, std::shared_ptr<Queue> queue);

        JobSystem* m_system;
        std::shared_ptr<Queue> m_queue = std::make_shared<Queue>();
    };

private:
    void Enqueue(std::function<void()> task);
    void WorkerLoop();

    mutable std::mutex m_mutex;
    std::condition_variable m_available;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_workers;
    bool m_stopping = false;
};

}  // namespace px::jobs
