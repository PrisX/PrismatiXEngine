#pragma once

#include "Engine/Progression/SaveSystem.h"

#include <algorithm>

namespace px::progress {

struct AwaitedWork {
    bool command = false;
    bool action = false;
    bool timeline = false;
    bool stageMotion = false;

    [[nodiscard]] bool Any() const { return command || action || timeline || stageMotion; }
};

// One definition of the external work that may legally suspend the VM. Hosts
// use this both when validating a checkpoint and when deciding whether the VM
// can resume, so commands, UI actions, and awaited timelines cannot drift.
[[nodiscard]] inline AwaitedWork InspectAwaitedWork(
    const SaveSnapshot& snapshot) {
    return AwaitedWork{
        .command = !snapshot.scriptPending.empty(),
        .action = !snapshot.scriptActions.empty(),
        .timeline = std::ranges::any_of(
            snapshot.timelines,
            [](const animation::PlaybackState& playback) {
                return playback.playing && playback.awaiting;
            }),
        .stageMotion = snapshot.stage.motion &&
                       snapshot.stage.motion->playing &&
                       snapshot.stage.motion->awaiting,
    };
}

[[nodiscard]] inline bool HasConsistentAwaitState(
    const SaveSnapshot& snapshot) {
    return (snapshot.vm.state == vn::VMState::WaitingExternal) ==
           InspectAwaitedWork(snapshot).Any();
}

}  // namespace px::progress
