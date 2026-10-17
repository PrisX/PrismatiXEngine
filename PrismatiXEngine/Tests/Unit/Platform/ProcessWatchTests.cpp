#include "Engine/Platform/ProcessWatch.h"
#include "Tests/TestSupport/TestHarness.h"

#include <atomic>
#include <chrono>
#include <thread>

int main() {
    px::test::Suite suite("ProcessWatch");
    using namespace std::chrono_literals;

    suite.Run("TheRunningParentIsAlive", [&] {
        // ctest (or the shell) started this test and outlives it.
        const auto parent = px::platform::ParentProcessId();
        suite.Require(parent.has_value(), "the parent process id is known");
        const auto alive = px::platform::ProcessAliveProbe(*parent);
        suite.Require(static_cast<bool>(alive), "the parent can be watched");
        suite.Expect(alive(), "a running parent reports alive");
    });

    suite.Run("AnUnusedProcessIdIsGone", [&] {
        const auto alive = px::platform::ProcessAliveProbe(0x7ffffff0ull);
        suite.Expect(!alive || !alive(), "an id that names no process is not alive");
        suite.Expect(!px::platform::ProcessAliveProbe(0), "id 0 cannot be watched");
    });

    suite.Run("ExitIsReportedOnce", [&] {
        std::atomic<bool> running{true};
        std::atomic<int> exits{0};
        auto watch = px::platform::ProcessExitWatch::Start(
            [&running] { return running.load(); }, [&exits] { ++exits; });
        suite.Require(watch != nullptr, "the watch starts");
        std::this_thread::sleep_for(300ms);
        suite.Expect(exits.load() == 0, "nothing is reported while the process runs");
        running = false;
        for (int wait = 0; wait < 40 && exits.load() == 0; ++wait) std::this_thread::sleep_for(50ms);
        suite.Expect(exits.load() == 1, "the exit is reported within a second");
        std::this_thread::sleep_for(600ms);
        suite.Expect(exits.load() == 1, "and only once");
    });

    suite.Run("StoppingTheWatchReportsNothing", [&] {
        std::atomic<int> exits{0};
        {
            auto watch = px::platform::ProcessExitWatch::Start([] { return true; }, [&exits] { ++exits; });
            suite.Require(watch != nullptr, "the watch starts");
        }
        suite.Expect(exits.load() == 0, "a destroyed watch never calls back");
        suite.Expect(px::platform::ProcessExitWatch::Start({}, [] {}) == nullptr,
                     "no probe, no watch");
    });

    return suite.Finish();
}
