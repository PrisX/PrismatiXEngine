#pragma once

#include "Engine/UI/Actions/ActionDescriptor.h"
#include "Engine/VN/Commands/Command.h"

#include <cstdint>
#include <string>
#include <vector>

namespace px::script {

// Language-neutral continuation checkpoints. Script VM memory is deliberately
// not persisted: hosts reconstruct deterministic work to an explicit await
// boundary and then restore the engine-owned wait token.
struct PendingCommandState {
    vn::Command command;
    std::uint32_t yieldIndex = 0;
    std::string waitKind;
    std::uint64_t handle = 0;
    float remainingSeconds = 0.0f;
};

using PendingCommandsState = std::vector<PendingCommandState>;

struct PendingActionState {
    std::uint64_t id = 0;
    ui::ActionInvocation invocation;
    std::uint32_t yieldIndex = 0;
    std::string waitKind;
    std::uint64_t handle = 0;
    float remainingSeconds = 0.0f;
};

using PendingActionsState = std::vector<PendingActionState>;

}  // namespace px::script
