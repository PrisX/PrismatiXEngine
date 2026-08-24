#pragma once

#include "Engine/SDK/Ui.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/VisualState.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::ui {

class Control;

struct UiAdapterDiagnostic {
    std::string code;
    std::string message;
    std::string nodeId;
};

struct UiRuntimeTree {
    std::unique_ptr<Control> root;
    std::size_t nodeCount = 0;
    std::size_t actionBindingCount = 0;
    std::size_t behaviorNodeCount = 0;
    std::size_t behaviorTriggerCount = 0;
    std::size_t animationClipCount = 0;
    std::size_t animationTrackCount = 0;
    std::size_t visualStateGroupCount = 0;
    std::size_t visualStateCount = 0;
    std::optional<BehaviorGraph> behaviorGraph;
    std::vector<TriggerBinding> behaviorTriggers;
    std::optional<UIAnimationLibrary> animations;
    std::vector<VisualStateGroup> visualStateGroups;
    std::vector<std::string> unresolvedAssetIds;
    std::vector<UiAdapterDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return root != nullptr && diagnostics.empty(); }
};

using UiAssetResolver =
    std::function<std::optional<std::string>(std::string_view assetId)>;
using UiActionSink =
    std::function<void(const sdk::UiAction& action,
                       std::string_view signal, std::string_view nodeId)>;

// Converts the public SDK contract into actual Runtime UI Controls. This is a
// Runtime adapter; the authored JSON contract remains independent of Engine UI
// implementation classes.
[[nodiscard]] UiRuntimeTree BuildUiRuntimeTree(
    const sdk::UiDocument& document,
    UiAssetResolver assetResolver = {},
    UiActionSink actionSink = {});

}  // namespace px::ui
