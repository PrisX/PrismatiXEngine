#include "Engine/Preview/PerformancePreview.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/Session/RuntimeIrAdapter.h"
#include "Tests/TestSupport/TestHarness.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <ctime>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// The budgets bound the work the code does, so they are checked against the
// CPU time this process used. Wall time also counts the time other processes
// held the CPU, which made the budgets fail on a loaded machine (a parallel
// ctest run) without the code getting slower; it is still printed.
std::chrono::nanoseconds ProcessCpuTime() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return std::chrono::nanoseconds{0};
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return std::chrono::nanoseconds{(ticks(kernel) + ticks(user)) * 100};
#else
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) return std::chrono::nanoseconds{0};
    return std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec};
#endif
}

struct Measured {
    std::chrono::nanoseconds cpu;
    Clock::duration wall;
};

template <typename Work>
Measured Measure(Work&& work) {
    const auto cpuStart = ProcessCpuTime();
    const auto wallStart = Clock::now();
    work();
    return {ProcessCpuTime() - cpuStart, Clock::now() - wallStart};
}

long long Milliseconds(const auto duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

#ifdef _DEBUG
constexpr auto kColdCompileBudget = std::chrono::milliseconds(8'000);
constexpr auto kSamplingBudget = std::chrono::milliseconds(7'000);
#else
constexpr auto kColdCompileBudget = std::chrono::milliseconds(2'000);
constexpr auto kSamplingBudget = std::chrono::milliseconds(1'500);
#endif

}  // namespace

int main() {
    px::test::Suite suite("PreviewPerformanceBudgets");

    suite.Run("TenThousandOperationColdCompile", [&] {
        constexpr std::size_t operationCount = 10'000;
        px::sdk::RuntimeIrDocument document;
        document.documentId = "large-story";
        document.committedRevision = 1;
        document.operations.reserve(operationCount);
        for (std::size_t index = 0; index < operationCount; ++index) {
            px::sdk::RuntimeIrOperation operation;
            operation.operationId = "operation-" + std::to_string(index);
            operation.sourceId = "block-" + std::to_string(index);
            operation.sourceLine = static_cast<std::uint32_t>(index + 1);
            operation.kind = "narration";
            operation.text = "Large Story line";
            operation.arguments.emplace("text", "Large Story line " +
                                                    std::to_string(index));
            document.operations.push_back(std::move(operation));
        }

        decltype(px::CompileRuntimeIr(document)) program;
        const auto elapsed = Measure([&] { program = px::CompileRuntimeIr(document); });
        suite.Expect(program.errors.empty(),
                     "large Runtime IR cold compile produces no diagnostics");
        suite.Expect(program.code.size() == operationCount,
                     "large Runtime IR preserves every operation",
                     std::to_string(program.code.size()));
        suite.Expect(elapsed.cpu < kColdCompileBudget,
                     "large Runtime IR cold compile stays inside its CI budget",
                     std::to_string(Milliseconds(elapsed.cpu)) + " ms CPU");
        std::cout << "10,000 operation cold compile: " << Milliseconds(elapsed.cpu)
                  << " ms CPU, " << Milliseconds(elapsed.wall) << " ms wall\n";
    });

    suite.Run("DenseTimelineSamplingMaintainsFrameBudget", [&] {
        constexpr std::size_t nodeCount = 256;
        constexpr std::size_t trackCount = 2'048;
        constexpr std::size_t sampleCount = 600;
        px::preview::PerformancePreviewSequence sequence;
        sequence.sceneId = "dense-performance";
        sequence.revision = 1;
        sequence.duration = 10.0;
        sequence.baseNodes.reserve(nodeCount);
        for (std::size_t index = 0; index < nodeCount; ++index) {
            px::preview::PerformancePreviewNode node;
            node.id = "node-" + std::to_string(index);
            node.kind = "character";
            sequence.baseNodes.push_back(std::move(node));
        }
        sequence.numericTracks.reserve(trackCount);
        for (std::size_t index = 0; index < trackCount; ++index) {
            px::preview::PerformancePreviewNumericTrack track;
            track.targetId = "node-" + std::to_string(index % nodeCount);
            track.property = index % 2 == 0 ? "x" : "y";
            track.keyframes = {
                {0.0, 0.0, "linear"},
                {5.0, 50.0, "linear"},
                {10.0, 100.0, "linear"},
            };
            sequence.numericTracks.push_back(std::move(track));
        }

        std::size_t sampledNodes = 0;
        double terminalX = 0.0;
        double terminalY = 0.0;
        const auto elapsed = Measure([&] {
            for (std::size_t frame = 0; frame < sampleCount; ++frame) {
                const auto plan = sequence.Sample(
                    static_cast<double>(frame) / 60.0);
                sampledNodes += plan.nodes.size();
                terminalX = plan.nodes.front().x;
                terminalY = plan.nodes[1].y;
            }
        });
        suite.Expect(sampledNodes == sampleCount * nodeCount,
                     "dense sampling returns every resolved node state",
                     std::to_string(sampledNodes));
        suite.Expect(terminalX > 99.0 && terminalY > 99.0,
                     "dense sampling resolves x and y tracks into the final frame");
        suite.Expect(elapsed.cpu < kSamplingBudget,
                     "600 dense Preview frames stay inside the CI budget",
                     std::to_string(Milliseconds(elapsed.cpu)) + " ms CPU");
        std::cout << "600 frames / 2,048 tracks: " << Milliseconds(elapsed.cpu)
                  << " ms CPU, " << Milliseconds(elapsed.wall) << " ms wall\n";
    });

    return suite.Finish();
}
