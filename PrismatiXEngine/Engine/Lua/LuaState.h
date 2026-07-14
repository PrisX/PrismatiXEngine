#pragma once

#include "Engine/VN/Commands/Command.h"
#include "Engine/UI/Actions/ActionDescriptor.h"

#include <cstdint>
#include <string>
#include <vector>

namespace px::lua {

// Serializable checkpoint for an extension command. Lua VM memory itself is
// intentionally not persisted: a command is deterministically reconstructed
// to its Nth declared await boundary, then the saved wait token is restored.
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

}  // namespace px::lua
