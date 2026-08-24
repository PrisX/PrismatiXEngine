#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
    std::size_t propertyBindingCount = 0;
};

struct UiComponentSource {
    std::string sourcePath;
    std::string json;
};

using UiComponentLoader = std::function<std::optional<UiComponentSource>(std::string_view)>;

struct UiApplicationOptions {
    std::string sourcePath;
    UiAssetResolver resolveAsset;
    UiComponentLoader loadComponent;
    IViewModel* viewModel = nullptr;
    bool previewSafeMode = false;
    bool diagnosticOverlay = false;
    std::function<void(const sdk::UiAction&, const Status&)> observeAction;
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
