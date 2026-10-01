#include "Engine/Core/JobSystem.h"
#include "Tests/TestSupport/TestHarness.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using px::jobs::Job;
using px::jobs::JobCancelled;
using px::jobs::JobSystem;

// Blocks the pool's worker until released, so tests control what is queued.
struct Gate {
    std::promise<void> promise;
    std::shared_future<void> opened = promise.get_future().share();
    void Open() { promise.set_value(); }
};

}  // namespace

int main() {
    px::test::Suite suite("JobSystem");

    suite.Run("DefaultPoolIsBounded", [&] {
        const auto workers = JobSystem::DefaultWorkerCount();
        suite.Expect(workers >= 2 && workers <= 8, "hardware_concurrency - 1, clamped to 2..8");
        suite.Expect(JobSystem::Shared().WorkerCount() == workers, "the shared pool uses it");
    });

    suite.Run("ResultsAndExceptions", [&] {
        JobSystem pool(2);
        auto value = pool.Submit([] { return 6 * 7; });
        auto text = pool.Submit([] { return std::string("ok"); });
        std::atomic<bool> ran = false;
        auto nothing = pool.Submit([&] { ran = true; });
        auto failing = pool.Submit([]() -> int { throw std::runtime_error("broken read"); });
        suite.Expect(value.get() == 42 && text.get() == "ok", "values come back");
        nothing.get();
        suite.Expect(ran.load(), "void jobs run");
        std::string message;
        try {
            (void)failing.get();
        } catch (const std::runtime_error& error) {
            message = error.what();
        }
        suite.Equal("broken read", message, "the job's exception is rethrown by get()");
        suite.Expect(!value.valid(), "get() consumes the handle");
    });

    suite.Run("ConcurrencyNeverExceedsWorkers", [&] {
        JobSystem pool(2);
        std::atomic<int> active = 0;
        std::atomic<int> peak = 0;
        std::atomic<int> finished = 0;
        std::vector<Job<int>> jobs;
        for (int index = 0; index < 24; ++index)
            jobs.push_back(pool.Submit([&, index] {
                const int now = ++active;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
                std::this_thread::sleep_for(2ms);
                --active;
                ++finished;
                return index;
            }));
        // Poll rather than wait(): waiting would run queued jobs on this
        // thread, which is intended but is not what this case measures.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (finished.load() < 24 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        int sum = 0;
        for (auto& job : jobs) sum += job.get();
        suite.Expect(sum == 24 * 23 / 2, "every job ran exactly once");
        suite.Expect(peak.load() <= 2, "a burst of 24 jobs uses at most the two workers");
    });

    suite.Run("QueuedJobsCancelWithoutRunning", [&] {
        JobSystem pool(1);
        Gate gate;
        auto blocker = pool.Submit([opened = gate.opened] { opened.wait(); });
        std::atomic<bool> cancelledRan = false;
        std::atomic<bool> droppedRan = false;
        auto cancelled = pool.Submit([&] { cancelledRan = true; return 1; });
        {
            auto dropped = pool.Submit([&] { droppedRan = true; });
        }
        suite.Expect(cancelled.Cancel() && cancelled.Cancelled() && cancelled.Ready(),
                     "a queued job cancels at once");
        bool threw = false;
        try {
            (void)cancelled.get();
        } catch (const JobCancelled&) {
            threw = true;
        }
        suite.Expect(threw, "get() on a cancelled job throws JobCancelled");
        gate.Open();
        blocker.get();
        auto fence = pool.Submit([] {});
        fence.get();
        suite.Expect(!cancelledRan && !droppedRan,
                     "neither a cancelled nor a dropped queued job ever runs");
    });

    suite.Run("RunningJobsSeeStopAndDroppingWaits", [&] {
        JobSystem pool(1);
        std::promise<void> started;
        auto startedFuture = started.get_future();
        auto loop = pool.Submit([&started](std::stop_token stop) {
            started.set_value();
            int spins = 0;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(1ms);
                if (++spins > 5000) return false;
            }
            return true;
        });
        startedFuture.wait();
        suite.Expect(!loop.Cancel(), "a running job cannot be un-run");
        suite.Expect(loop.get(), "but it observes the stop request");

        std::atomic<bool> finished = false;
        std::promise<void> running;
        auto runningFuture = running.get_future();
        {
            auto slow = pool.Submit([&] {
                running.set_value();
                std::this_thread::sleep_for(30ms);
                finished = true;
            });
            runningFuture.wait();
        }
        suite.Expect(finished.load(),
                     "dropping a running job waits, so it cannot outlive what it uses");
    });

    suite.Run("GetRunsAQueuedJobOnTheWaitingThread", [&] {
        JobSystem pool(1);
        Gate gate;
        auto blocker = pool.Submit([opened = gate.opened] { opened.wait(); });
        const auto caller = std::this_thread::get_id();
        auto queued = pool.Submit([] { return std::this_thread::get_id(); });
        const auto ranOn = queued.get();
        suite.Expect(ranOn == caller,
                     "the main thread does not queue behind unrelated work");
        gate.Open();
        blocker.get();
    });

    suite.Run("StrandKeepsOrderAndNeverOverlaps", [&] {
        JobSystem pool(4);
        JobSystem::Strand strand(pool);
        std::mutex mutex;
        std::vector<int> order;
        std::atomic<int> inFlight = 0;
        std::atomic<bool> overlapped = false;
        std::vector<Job<void>> jobs;
        for (int index = 0; index < 40; ++index)
            jobs.push_back(strand.Submit([&, index] {
                if (++inFlight > 1) overlapped = true;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                {
                    std::scoped_lock lock(mutex);
                    order.push_back(index);
                }
                --inFlight;
            }));
        for (auto& job : jobs) job.get();
        bool ascending = order.size() == 40;
        for (std::size_t index = 0; ascending && index < order.size(); ++index)
            ascending = order[index] == static_cast<int>(index);
        suite.Expect(ascending, "strand jobs finish in submission order");
        suite.Expect(!overlapped.load(), "one strand job at a time, even with four workers");
    });

    suite.Run("StrandGetDoesNotJumpTheQueue", [&] {
        JobSystem pool(2);
        JobSystem::Strand strand(pool);
        Gate gate;
        std::mutex mutex;
        std::vector<std::string> order;
        auto first = strand.Submit([&, opened = gate.opened] {
            opened.wait();
            std::scoped_lock lock(mutex);
            order.push_back("settings");
        });
        auto second = strand.Submit([&] {
            std::scoped_lock lock(mutex);
            order.push_back("profile");
            return true;
        });
        std::thread opener([&gate] {
            std::this_thread::sleep_for(30ms);
            gate.Open();
        });
        suite.Expect(second.get(), "the later write completes");
        opener.join();
        first.get();
        suite.Expect(order == std::vector<std::string>{"settings", "profile"},
                     "waiting on a later write never runs it before an earlier one");
    });

    suite.Run("ZeroWorkersRunInline", [&] {
        JobSystem pool(0);
        const auto caller = std::this_thread::get_id();
        auto job = pool.Submit([] { return std::this_thread::get_id(); });
        suite.Expect(job.Ready() && job.get() == caller, "single-threaded targets run at Submit");
        JobSystem::Strand strand(pool);
        int value = 0;
        auto first = strand.Submit([&] { value = value * 10 + 1; });
        auto second = strand.Submit([&] { value = value * 10 + 2; });
        suite.Expect(value == 12 && first.Ready() && second.Ready(),
                     "strands also run inline, in order");
    });

    suite.Run("DestroyingThePoolFinishesQueuedJobs", [&] {
        std::vector<Job<int>> jobs;
        {
            JobSystem pool(1);
            for (int index = 0; index < 8; ++index)
                jobs.push_back(pool.Submit([index] {
                    std::this_thread::sleep_for(1ms);
                    return index;
                }));
        }
        bool allReady = true;
        for (auto& job : jobs) allReady = allReady && job.Ready();
        suite.Expect(allReady && jobs.back().get() == 7,
                     "no handle is left waiting on a destroyed pool");
    });

    return suite.Finish();
}
