#pragma once

#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/Control.h"

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
    std::optional<AnimationClip> animation;
    std::optional<Theme> theme;
};

[[nodiscard]] Result<LoadedUIScene> InstantiateUIScene(const resource::TypedDocument& document,
                                                       IViewModel* viewModel,
                                                       const FormatterRegistry& formatters);
[[nodiscard]] Result<LoadedUIScene> LoadUIScene(const std::filesystem::path& path,
                                                IViewModel* viewModel,
                                                const FormatterRegistry& formatters);

}  // namespace px::ui
