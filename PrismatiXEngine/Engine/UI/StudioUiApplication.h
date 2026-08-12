#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "Engine/Core/Result.h"
#include "Engine/SDK/StudioUi.h"
#include "Engine/UI/StudioUiAdapter.h"

namespace px::ui {

class UIContext;
class IViewModel;

struct StudioUiApplicationSummary {
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

struct StudioUiComponentSource {
    std::string sourcePath;
    std::string json;
};

using StudioUiComponentLoader = std::function<std::optional<StudioUiComponentSource>(std::string_view)>;

struct StudioUiApplicationOptions {
    std::string sourcePath;
    StudioUiAssetResolver resolveAsset;
    StudioUiComponentLoader loadComponent;
    IViewModel* viewModel = nullptr;
    bool previewSafeMode = false;
    bool diagnosticOverlay = false;
    std::function<void(const sdk::StudioUiAction&, const Status&)> observeAction;
};

// Production application path for Studio-authored UI. PreviewHost and Player
// both use this service so parsing, layout, Actions, Behavior and Animation
// cannot silently diverge between authoring preview and packaged playback.
class StudioUiApplication {
public:
    explicit StudioUiApplication(UIContext& context) : m_context(context) {}

    [[nodiscard]] Result<StudioUiApplicationSummary> ApplyText(std::string_view json, StudioUiApplicationOptions options = {});
    [[nodiscard]] Result<StudioUiApplicationSummary> ApplyDocument(const sdk::StudioUiDocument& document, StudioUiApplicationOptions options = {});
    // Updates authored control properties without replacing the installed
    // Runtime tree. The caller must use a reload plan for topology,
    // interaction, binding, Behavior, Animation, theme, or component changes.
    [[nodiscard]] Result<StudioUiApplicationSummary> PatchDocumentProperties(const sdk::StudioUiDocument& document, StudioUiApplicationOptions options = {});

private:
    UIContext& m_context;
};

}  // namespace px::ui
