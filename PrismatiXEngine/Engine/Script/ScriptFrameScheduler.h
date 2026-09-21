#pragma once

#include <functional>

namespace px::script {

class ScriptHost;

// Advances the extension clock exactly once for an application frame. Host
// applications provide their own safe-point queue and continuation policy,
// while sharing the ordering and frame.update event contract.
void AdvanceScriptFrame(ScriptHost& host, float deltaSeconds,
                        const std::function<void()>& safePoint = {},
                        const std::function<void()>& onPendingDrained = {});

}  // namespace px::script
