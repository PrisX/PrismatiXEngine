#pragma once

#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/VisualState.h"

#include <optional>

namespace px::ui {

// Complete deterministic UI checkpoint. Optional controller state preserves
// whether the applied UI document actually configured that subsystem.
struct UIRuntimeState {
    BehaviorRuntimeState behavior;
    std::optional<UIAnimationRuntimeState> animation;
    std::optional<VisualStateRuntimeState> visualState;
};

}  // namespace px::ui
