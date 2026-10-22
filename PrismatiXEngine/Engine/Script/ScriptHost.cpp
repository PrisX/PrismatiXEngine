#include "Engine/Script/ScriptHost.h"

#include "Engine/Animation/Timeline.h"
#include "Engine/Script/JavaScriptHost.h"
#include "Engine/Diagnostics/Diagnostic.h"

namespace px::script {

Status ScriptHost::RestoreCheckpoint(
    const PendingCommandsState& commands, const PendingActionsState& actions,
    const std::optional<CancellationReason> replaced) {
    const PendingCommandsState previousCommands = CapturePending();
    const PendingActionsState previousActions = CapturePendingActions();
    // The running handlers end before the checkpoint replays: their abort
    // listeners and finally blocks run now, and the restored state that
    // follows overwrites whatever they wrote. If the restore then fails, the
    // rollback below replays them from their own checkpoint.
    if (replaced) CancelPending(*replaced);
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

void BindTimelineMarkerEvents(animation::TimelinePlayer& timeline,
                              std::function<ScriptHost*()> host) {
    timeline.SetDetailedEvent(
        [host = std::move(host)](const animation::MarkerEvent& event) {
            ScriptHost* const scriptHost = host ? host() : nullptr;
            if (!scriptHost) return;
            (void)scriptHost->Emit(
                "timeline.marker",
                Variant(VariantObject{
                    {"handle", static_cast<std::int64_t>(event.handle)},
                    {"playbackClip", event.playbackClip.ToString()},
                    {"clip", event.clip.ToString()},
                    {"clipName", event.clipName},
                    {"marker", event.marker.name},
                    {"markerId", event.marker.id},
                    {"loopIteration",
                     static_cast<std::int64_t>(event.loopIteration)},
                    {"playbackLoopIteration",
                     static_cast<std::int64_t>(
                         event.playbackLoopIteration)},
                    {"payload", Variant(event.marker.payload)}}));
        });
}

}  // namespace px::script
