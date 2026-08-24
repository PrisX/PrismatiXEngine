#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px {
class RuntimeSession;
namespace ui {
class GalgameUI;
}
}

namespace px::vn {
class GameCatalog;
}

namespace px::preview {

struct PerformancePreviewNode {
    std::string id;
    std::string kind;
    std::string imagePath;
    float x = 0.0f;
    float y = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotation = 0.0f;
    float opacity = 1.0f;
    int zOrder = 0;
    bool visible = true;
};

struct PerformancePreviewProperty {
    std::string targetId;
    std::string property;
    double value = 0.0;
};

struct PerformancePreviewBackground {
    std::string clipId;
    std::string imagePath;
    bool crossfade = false;
};

struct PerformancePreviewAudioChannel {
    bool managed = false;
    bool playing = false;
    std::string clipId;
    std::string path;
    bool loop = false;
    int volume = 128;
    double offsetSeconds = 0.0;
};

struct PerformancePreviewAudioPlan {
    PerformancePreviewAudioChannel music;
    PerformancePreviewAudioChannel ambience;
    PerformancePreviewAudioChannel voice;
    std::string signature;
};

struct PerformancePreviewCharacterState {
    std::string targetId;
    std::string imagePath;
    int slot = 2;
    bool visible = false;
    bool crossfade = false;
};

enum class PerformancePreviewUiOperation : std::uint8_t {
    Show,
    Hide,
    PlayAnimation,
};

struct PerformancePreviewUiControlState {
    std::string clipId;
    std::string targetId;
    bool visible = true;
};

struct PerformancePreviewUiAnimationState {
    std::string clipId;
    std::string animationClipId;
    double offsetSeconds = 0.0;
    bool playing = false;
};

struct PerformancePreviewPlan {
    std::string sceneId;
    std::string uiSceneId;
    std::uint64_t revision = 0;
    double seekTime = 0.0;
    std::vector<PerformancePreviewNode> nodes;
    std::vector<PerformancePreviewProperty> properties;
    std::optional<PerformancePreviewBackground> background;
    PerformancePreviewAudioPlan audio;
    std::vector<PerformancePreviewCharacterState> characters;
    std::vector<PerformancePreviewUiControlState> uiControls;
    std::optional<PerformancePreviewUiAnimationState> uiAnimation;
    bool managesUiAnimation = false;
    std::size_t unsafeEventsSkipped = 0;
};

struct PerformancePreviewKeyframe {
    double time = 0.0;
    double value = 0.0;
    std::string curve = "linear";
};

struct PerformancePreviewNumericTrack {
    std::string targetId;
    std::string property;
    double fallback = 0.0;
    std::vector<PerformancePreviewKeyframe> keyframes;
};

struct PerformancePreviewBackgroundClip {
    std::string id;
    double start = 0.0;
    double duration = 0.0;
    std::string imagePath;
    bool crossfade = false;
};

enum class PerformancePreviewAudioBus : std::uint8_t {
    Music,
    Ambience,
    Voice,
};

struct PerformancePreviewAudioClip {
    std::string id;
    PerformancePreviewAudioBus bus = PerformancePreviewAudioBus::Music;
    double start = 0.0;
    double duration = 0.0;
    std::string path;
    bool loop = false;
    int volume = 128;
    std::uint32_t fadeInMs = 0;
    std::uint32_t fadeOutMs = 0;
    bool stopAtEnd = true;
};

struct PerformancePreviewEvent {
    std::string id;
    double time = 0.0;
    std::string actionId;
    VariantObject arguments;
};

enum class PerformancePreviewCharacterOperation : std::uint8_t {
    Show,
    Expression,
    Hide,
};

struct PerformancePreviewCharacterClip {
    std::string id;
    std::string targetId;
    double start = 0.0;
    double duration = 0.0;
    PerformancePreviewCharacterOperation operation =
        PerformancePreviewCharacterOperation::Show;
    std::string imagePath;
    int slot = 2;
    bool crossfade = false;
};

struct PerformancePreviewUiClip {
    std::string id;
    double start = 0.0;
    double duration = 0.0;
    PerformancePreviewUiOperation operation =
        PerformancePreviewUiOperation::Show;
    // show/hide target a Control UUID; playAnimation targets an authored UI
    // animation clip UUID from the bound UI document scene.
    std::string targetId;
};

// Immutable, validated Performance data compiled once at apply time. Sampling
// this object during the browser main loop never reparses authored JSON or
// resolves project paths.
struct PerformancePreviewSequence {
    std::string sceneId;
    std::string uiSceneId;
    std::uint64_t revision = 0;
    double duration = 0.0;
    std::vector<PerformancePreviewNode> baseNodes;
    std::vector<PerformancePreviewNumericTrack> numericTracks;
    std::vector<PerformancePreviewBackgroundClip> backgroundClips;
    std::vector<PerformancePreviewAudioClip> audioClips;
    std::vector<PerformancePreviewEvent> events;
    std::vector<PerformancePreviewCharacterClip> characterClips;
    std::vector<PerformancePreviewUiClip> uiClips;
    bool managesUiAnimation = false;
    bool managesMusic = false;
    bool managesAmbience = false;
    bool managesVoice = false;

    [[nodiscard]] PerformancePreviewPlan Sample(double time) const;
    [[nodiscard]] std::vector<PerformancePreviewEvent> EventsBetween(
        double fromTime, double toTime) const;
};

using PreviewResourceExists = std::function<bool(std::string_view)>;

[[nodiscard]] Result<PerformancePreviewSequence> BuildPerformancePreviewSequence(
    std::string_view performanceJson, std::string_view projectManifestJson,
    const vn::GameCatalog& catalog, std::string_view expectedSceneId,
    const PreviewResourceExists& exists = {});

// Parses, validates, resolves and deterministically samples a Performance v2
// document without mutating Runtime state. Both Native Check and WASM Preview
// can therefore compare the exact same plan before installing a frame.
[[nodiscard]] Result<PerformancePreviewPlan> BuildPerformancePreviewPlan(
    std::string_view performanceJson, std::string_view projectManifestJson,
    const vn::GameCatalog& catalog, std::string_view expectedSceneId,
    double seekTime, const PreviewResourceExists& exists = {});

void ApplyPerformancePreviewPlan(RuntimeSession& session,
                                 const PerformancePreviewPlan& plan,
                                 bool resetStage = true,
                                 bool restoreAudioTracks = true);

[[nodiscard]] Status ApplyPerformancePreviewUiPlan(
    ui::GalgameUI& ui, const PerformancePreviewPlan& plan);

}  // namespace px::preview
