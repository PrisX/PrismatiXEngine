#pragma once

#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UIResourceResolver.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <filesystem>
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

[[nodiscard]] Result<LoadedUIScene> InstantiateUIScene(const resource::TypedDocument& document,
                                                       IViewModel* viewModel,
                                                       const FormatterRegistry& formatters);
[[nodiscard]] Result<LoadedUIScene> InstantiateUIScene(const resource::TypedDocument& document,
                                                       IViewModel* viewModel,
                                                       const FormatterRegistry& formatters,
                                                       const UIDocumentLoader& loader);
[[nodiscard]] Result<LoadedUIScene> LoadUIScene(const std::filesystem::path& path,
                                                IViewModel* viewModel,
                                                const FormatterRegistry& formatters);

}  // namespace px::ui
