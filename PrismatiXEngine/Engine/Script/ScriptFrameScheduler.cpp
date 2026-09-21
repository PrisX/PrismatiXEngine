#include "Engine/Script/ScriptFrameScheduler.h"

#include "Engine/Script/ScriptHost.h"

#include <string>

namespace px::script {

void AdvanceScriptFrame(ScriptHost& host, const float deltaSeconds,
                        const std::function<void()>& safePoint,
                        const std::function<void()>& onPendingDrained) {
    (void)host.Emit("frame.update",
                    {{"delta", std::to_string(deltaSeconds)}});
    const bool hadPending =
        host.HasPendingCommand() || host.HasPendingAction();
    host.Update(deltaSeconds);
    if (safePoint) safePoint();
    if (hadPending && !host.HasPendingCommand() &&
        !host.HasPendingAction() && onPendingDrained)
        onPendingDrained();
}

}  // namespace px::script
