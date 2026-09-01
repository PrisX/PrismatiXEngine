#pragma once

#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/VisualState.h"

#include <optional>
#include <string>

namespace px::ui {

// Complete deterministic UI checkpoint. Optional controller state preserves
// whether the applied UI document actually configured that subsystem.
struct UIRuntimeState {
    // Stable package UI surface identity.  A checkpoint is only meaningful
    // against the document/controller topology that produced it.
    std::string surfaceId = "hud";
    BehaviorRuntimeState behavior;
    std::optional<UIAnimationRuntimeState> animation;
    std::optional<VisualStateRuntimeState> visualState;
};

}  // namespace px::ui
