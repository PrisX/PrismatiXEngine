#pragma once

#include "Engine/SDK/StudioUi.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::ui {

class Control;

struct StudioUiAdapterDiagnostic {
    std::string code;
    std::string message;
    std::string nodeId;
};

struct StudioUiRuntimeTree {
    std::unique_ptr<Control> root;
    std::size_t nodeCount = 0;
    std::size_t actionBindingCount = 0;
    std::size_t behaviorNodeCount = 0;
    std::size_t behaviorTriggerCount = 0;
    std::size_t animationClipCount = 0;
    std::size_t animationTrackCount = 0;
    std::optional<BehaviorGraph> behaviorGraph;
    std::vector<TriggerBinding> behaviorTriggers;
    std::optional<UIAnimationLibrary> animations;
    std::vector<std::string> unresolvedAssetIds;
    std::vector<StudioUiAdapterDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return root != nullptr && diagnostics.empty(); }
};

using StudioUiAssetResolver =
    std::function<std::optional<std::string>(std::string_view assetId)>;
using StudioUiActionSink =
    std::function<void(const sdk::StudioUiAction& action,
                       std::string_view signal, std::string_view nodeId)>;

// Converts the public SDK contract into actual Runtime UI Controls. This is a
// Runtime adapter; Studio's JSON contract remains independent of Engine UI
// implementation classes.
[[nodiscard]] StudioUiRuntimeTree BuildStudioUiRuntimeTree(
    const sdk::StudioUiDocument& document,
    StudioUiAssetResolver assetResolver = {},
    StudioUiActionSink actionSink = {});

}  // namespace px::ui
