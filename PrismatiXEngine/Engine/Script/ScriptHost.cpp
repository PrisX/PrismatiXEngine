#include "Engine/Script/ScriptHost.h"

#include "Engine/Script/JavaScriptHost.h"
#include "Engine/Diagnostics/Diagnostic.h"

namespace px::script {

Status ScriptHost::RestoreCheckpoint(const PendingCommandsState& commands,
                                     const PendingActionsState& actions) {
    const PendingCommandsState previousCommands = CapturePending();
    const PendingActionsState previousActions = CapturePendingActions();
    const auto rollback = [this, &previousCommands, &previousActions] {
        const Status commandsRestored = RestorePending(previousCommands);
        const Status actionsRestored = RestorePendingActions(previousActions);
        if (!commandsRestored || !actionsRestored) {
            diag::Emit(diag::Diagnostic{
                .severity = diag::Severity::Fatal,
                .code = "PXJS7515",
                .category = "Script.Restore",
                .message = "JavaScript checkpoint rollback could not recover the active realms"});
            return false;
        }
        return true;
    };
    if (const Status restored = RestorePending(commands); !restored) {
        (void)rollback();
        return restored;
    }
    if (const Status restored = RestorePendingActions(actions); !restored) {
        (void)rollback();
        return restored;
    }
    return Status::Ok();
}

std::unique_ptr<ScriptHost> CreateScriptHost(const ScriptServices& services) {
    return std::make_unique<JavaScriptHost>(services);
}

}  // namespace px::script
