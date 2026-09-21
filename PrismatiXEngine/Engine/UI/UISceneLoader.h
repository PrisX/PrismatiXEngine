#pragma once

#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UIResourceResolver.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <memory>
#include <vector>
#include <optional>
#include "Engine/UI/Animation.h"
#include "Engine/UI/Theme.h"

namespace px::ui {

struct LoadedUIScene {
    std::unique_ptr<Control> root;
    std::vector<Binding> bindings;
    std::optional<UIAnimationLibrary> animations;
    std::optional<Theme> theme;
    std::vector<TriggerBinding> triggers;
    std::optional<BehaviorGraph> interactionGraph;
    std::string sourceScene;
};

// One-way compatibility adapter for migration tooling and legacy fixture
// verification. Production UI is installed through UiApplication.
[[nodiscard]] Result<LoadedUIScene> InstantiateLegacyUISceneForMigration(
    const resource::TypedDocument& document, IViewModel* viewModel,
    const FormatterRegistry& formatters);
[[nodiscard]] Result<LoadedUIScene> InstantiateLegacyUISceneForMigration(
    const resource::TypedDocument& document, IViewModel* viewModel,
    const FormatterRegistry& formatters, const UIDocumentLoader& loader);
}  // namespace px::ui
