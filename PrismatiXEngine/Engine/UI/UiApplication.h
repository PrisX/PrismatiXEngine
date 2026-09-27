#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Core/Result.h"
#include "Engine/SDK/Ui.h"
#include "Engine/UI/UiAdapter.h"

namespace px::ui {

class UIContext;
class IViewModel;

struct UiApplicationSummary {
    std::string documentId;
    std::uint64_t revision = 0;
    std::size_t nodeCount = 0;
    std::size_t actionBindingCount = 0;
    std::size_t behaviorNodeCount = 0;
    std::size_t behaviorTriggerCount = 0;
    std::size_t animationClipCount = 0;
    std::size_t animationTrackCount = 0;
    std::size_t visualStateGroupCount = 0;
    std::size_t visualStateCount = 0;
    std::size_t propertyBindingCount = 0;
};

struct UiComponentSource {
    std::string sourcePath;
    std::string json;
};

using UiComponentLoader = std::function<std::optional<UiComponentSource>(std::string_view)>;
// Historical name; also loads .pxportrait text for TextureRect.
using UiMotionLoader = std::function<std::optional<std::string>(std::string_view)>;

// Resolves authored components and layered motion/portrait descriptors before a UI
// scene is installed, so a host can preload its static image dependencies.
// Runtime bindings may introduce additional paths later.
[[nodiscard]] Result<std::vector<std::string>> CollectUiImageDependencies(
    const sdk::UiDocument& document, UiAssetResolver resolveAsset = {},
    UiComponentLoader loadComponent = {}, UiMotionLoader loadMotion = {},
    std::string sourcePath = {});

struct UiApplicationOptions {
    std::string sourcePath{};
    UiAssetResolver resolveAsset{};
    UiComponentLoader loadComponent{};
    UiMotionLoader loadMotion{};
    IViewModel* viewModel = nullptr;
    bool previewSafeMode = false;
    bool diagnosticOverlay = false;
    std::function<void(const sdk::UiAction&, const Status&)> observeAction{};
};

// Production application path for authored UI. PreviewHost and Player
// both use this service so parsing, layout, Actions, Behavior and Animation
// cannot silently diverge between authoring preview and packaged playback.
class UiApplication {
public:
    explicit UiApplication(UIContext& context) : m_context(context) {}

    [[nodiscard]] Result<UiApplicationSummary> ApplyText(std::string_view json, UiApplicationOptions options = {});
    [[nodiscard]] Result<UiApplicationSummary> ApplyDocument(const sdk::UiDocument& document, UiApplicationOptions options = {});
    // Updates authored control properties without replacing the installed
    // Runtime tree. The caller must use a reload plan for topology,
    // interaction, binding, Behavior, Animation, theme, or component changes.
    [[nodiscard]] Result<UiApplicationSummary> PatchDocumentProperties(const sdk::UiDocument& document, UiApplicationOptions options = {});

private:
    UIContext& m_context;
};

}  // namespace px::ui
