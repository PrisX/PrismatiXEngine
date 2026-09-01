#pragma once

#include "Engine/UI/Actions/ActionDescriptor.h"
#include "Engine/VN/Commands/Command.h"

#include <cstdint>
#include <string>
#include <vector>

namespace px::script {

struct EngineOperationJournalEntry {
    std::string operation;
    VariantArray arguments;
    Variant result;
    bool resultUndefined = false;
};

using EngineOperationJournal = std::vector<EngineOperationJournalEntry>;

// Language-neutral continuation checkpoints. Script VM memory is deliberately
// not persisted: hosts reconstruct deterministic work to an explicit await
// boundary and then restore the engine-owned wait token.
struct PendingCommandState {
    std::string sourceId;
    vn::Command command;
    std::uint32_t yieldIndex = 0;
    std::string waitKind;
    std::uint64_t handle = 0;
    float remainingSeconds = 0.0f;
    EngineOperationJournal journal;
};

using PendingCommandsState = std::vector<PendingCommandState>;

struct PendingActionState {
    std::string sourceId;
    std::uint64_t id = 0;
    ui::ActionInvocation invocation;
    std::uint32_t yieldIndex = 0;
    std::string waitKind;
    std::uint64_t handle = 0;
    float remainingSeconds = 0.0f;
    EngineOperationJournal journal;
};

using PendingActionsState = std::vector<PendingActionState>;

}  // namespace px::script
