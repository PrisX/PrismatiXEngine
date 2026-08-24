#include "Engine/Preview/PerformancePreview.h"

#include "Engine/VN/GameCatalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace px::preview {
namespace {

using Json = nlohmann::json;

diag::Diagnostic Problem(std::string code, std::string message,
                         const std::string_view path = {},
                         const std::string_view nodeId = {},
                         const std::string_view property = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "Preview.Performance",
                                .message = std::move(message)};
    diagnostic.source.path = path;
    diagnostic.source.nodeId = nodeId;
    diagnostic.source.property = property;
    return diagnostic;
}

bool FiniteNumber(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_number() &&
           std::isfinite(value->get<double>());
}

double NodeNumber(const Json& node, const char* key, const double fallback) {
    const auto value = node.find(key);
    return value != node.end() && value->is_number() &&
                   std::isfinite(value->get<double>())
               ? value->get<double>()
               : fallback;
}

std::string StringOrEmpty(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_string()
               ? value->get<std::string>()
               : std::string{};
}

bool KnownTrackKind(const std::string_view kind) {
    static constexpr std::array<std::string_view, 9> kinds = {
        "camera", "character", "characterTransform", "background", "audio",
        "voice", "effect", "ui", "event"};
    return std::ranges::find(kinds, kind) != kinds.end();
}

bool TransformProperty(const std::string_view property) {
    static constexpr std::array<std::string_view, 6> properties = {
        "x", "y", "scaleX", "scaleY", "rotation", "opacity"};
    return std::ranges::find(properties, property) != properties.end();
}

bool CameraProperty(const std::string_view property) {
    static constexpr std::array<std::string_view, 3> properties = {
        "x", "y", "zoom"};
    return std::ranges::find(properties, property) != properties.end();
}

bool EffectProperty(const std::string_view property) {
    static constexpr std::array<std::string_view, 6> properties = {
        "shake", "flash", "fade", "blur", "vignette", "color-grade"};
    return std::ranges::find(properties, property) != properties.end();
}

double GlobalPropertyFallback(const std::string_view kind,
                              const std::string_view property) {
    return kind == "camera" && property == "zoom" ? 1.0 : 0.0;
}

double CurveAlpha(const std::string_view curve, const double alpha) {
    if (curve == "step" || curve == "hold") return 0.0;
    if (curve == "easeInOut")
        return alpha * alpha * (3.0 - 2.0 * alpha);
    return alpha;
}

double SampleTrack(const PerformancePreviewNumericTrack& track,
                   const double time, const double fallback) {
    if (track.keyframes.empty()) return fallback;
    const auto after = std::ranges::upper_bound(
        track.keyframes, time, {}, &PerformancePreviewKeyframe::time);
    if (after == track.keyframes.begin()) return fallback;
    const auto before = std::prev(after);
    if (after == track.keyframes.end() || before->curve == "step" ||
        before->curve == "hold")
        return before->value;
    const double span = after->time - before->time;
    if (span <= 0.0) return before->value;
    const double alpha = CurveAlpha(
        before->curve,
        std::clamp((time - before->time) / span, 0.0, 1.0));
    return before->value + (after->value - before->value) * alpha;
}

std::optional<Variant> VariantFromJson(const Json& value,
                                       const std::size_t depth = 0) {
    if (depth > 12) return std::nullopt;
    if (value.is_null()) return Variant{};
    if (value.is_boolean()) return Variant(value.get<bool>());
    if (value.is_number_integer()) return Variant(value.get<std::int64_t>());
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return Variant(static_cast<std::int64_t>(number));
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        return std::isfinite(number) ? std::optional<Variant>{Variant(number)}
                                     : std::nullopt;
    }
    if (value.is_string()) return Variant(value.get<std::string>());
    if (value.is_array()) {
        if (value.size() > 128) return std::nullopt;
        VariantArray result;
        for (const auto& item : value) {
            auto converted = VariantFromJson(item, depth + 1);
            if (!converted) return std::nullopt;
            result.push_back(std::move(*converted));
        }
        return Variant(std::move(result));
    }
    if (value.is_object()) {
        if (value.size() > 128) return std::nullopt;
        VariantObject result;
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            auto converted = VariantFromJson(iterator.value(), depth + 1);
            if (!converted) return std::nullopt;
            result.emplace(iterator.key(), std::move(*converted));
        }
        return Variant(std::move(result));
    }
    return std::nullopt;
}

int SampleClipVolume(const PerformancePreviewAudioClip& clip,
                     const double time) {
    double gain = 1.0;
    const double offsetMs = std::max(0.0, time - clip.start) * 1000.0;
    if (clip.fadeInMs > 0)
        gain = std::min(gain, offsetMs / clip.fadeInMs);
    if (clip.fadeOutMs > 0 && clip.stopAtEnd) {
        const double remainingMs =
            std::max(0.0, clip.start + clip.duration - time) * 1000.0;
        gain = std::min(gain, remainingMs / clip.fadeOutMs);
    }
    return static_cast<int>(std::lround(
        std::clamp(gain, 0.0, 1.0) * static_cast<double>(clip.volume)));
}

bool Overlaps(const PerformancePreviewAudioClip& left,
              const PerformancePreviewAudioClip& right) {
    return left.start < right.start + right.duration &&
           right.start < left.start + left.duration;
}

}  // namespace

PerformancePreviewPlan PerformancePreviewSequence::Sample(
    const double requestedTime) const {
    PerformancePreviewPlan plan;
    plan.sceneId = sceneId;
    plan.uiSceneId = uiSceneId;
    plan.revision = revision;
    plan.seekTime = std::clamp(requestedTime, 0.0, duration);
    plan.managesUiAnimation = managesUiAnimation;
    plan.nodes = baseNodes;
    for (const auto& track : numericTracks) {
        if (track.targetId == "$camera") {
            plan.properties.push_back({track.targetId, track.property,
                                       SampleTrack(track, plan.seekTime,
                                                   track.fallback)});
        }
    }
    for (auto& node : plan.nodes) {
        for (const auto& track : numericTracks) {
            if (track.targetId != node.id) continue;
            if (track.property == "x")
                node.x = static_cast<float>(SampleTrack(track, plan.seekTime, node.x));
            else if (track.property == "y")
                node.y = static_cast<float>(SampleTrack(track, plan.seekTime, node.y));
            else if (track.property == "scaleX")
                node.scaleX = static_cast<float>(SampleTrack(track, plan.seekTime, node.scaleX));
            else if (track.property == "scaleY")
                node.scaleY = static_cast<float>(SampleTrack(track, plan.seekTime, node.scaleY));
            else if (track.property == "rotation")
                node.rotation = static_cast<float>(SampleTrack(track, plan.seekTime, node.rotation));
            else if (track.property == "opacity")
                node.opacity = static_cast<float>(SampleTrack(track, plan.seekTime, node.opacity));
        }
    }
    std::unordered_map<std::string, PerformancePreviewCharacterState>
        characterStates;
    for (const auto& clip : characterClips) {
        if (clip.start > plan.seekTime) break;
        auto& state = characterStates[clip.targetId];
        state.targetId = clip.targetId;
        state.slot = clip.slot;
        state.crossfade = clip.crossfade;
        if (clip.operation == PerformancePreviewCharacterOperation::Hide) {
            state.visible = false;
            state.imagePath.clear();
        } else {
            state.visible = true;
            state.imagePath = clip.imagePath;
        }
    }
    for (const auto& [targetId, state] : characterStates) {
        std::erase_if(plan.nodes,
                      [&](const auto& node) { return node.id == targetId; });
        plan.characters.push_back(state);
    }
    std::ranges::sort(plan.characters, {},
                      &PerformancePreviewCharacterState::targetId);
    std::unordered_map<std::string, PerformancePreviewUiControlState>
        uiControlStates;
    for (const auto& clip : uiClips) {
        if (clip.start > plan.seekTime) break;
        if (clip.operation == PerformancePreviewUiOperation::PlayAnimation) {
            plan.uiAnimation = PerformancePreviewUiAnimationState{
                clip.id,
                clip.targetId,
                std::clamp(plan.seekTime - clip.start, 0.0, clip.duration),
                plan.seekTime < clip.start + clip.duration};
            continue;
        }
        uiControlStates[clip.targetId] = PerformancePreviewUiControlState{
            clip.id, clip.targetId,
            clip.operation == PerformancePreviewUiOperation::Show};
    }
    for (auto& [targetId, state] : uiControlStates) {
        (void)targetId;
        plan.uiControls.push_back(std::move(state));
    }
    std::ranges::sort(plan.uiControls, {},
                      &PerformancePreviewUiControlState::targetId);
    std::erase_if(plan.nodes,
                  [](const auto& node) { return !node.visible; });
    for (const auto& clip : backgroundClips) {
        if (clip.start > plan.seekTime) break;
        plan.background = PerformancePreviewBackground{
            clip.id, clip.imagePath, clip.crossfade};
    }
    const auto sampleAudio = [&](const PerformancePreviewAudioBus bus,
                                 PerformancePreviewAudioChannel& channel) {
        channel.managed = bus == PerformancePreviewAudioBus::Music
                              ? managesMusic
                          : bus == PerformancePreviewAudioBus::Ambience
                              ? managesAmbience
                              : managesVoice;
        for (const auto& clip : audioClips) {
            if (clip.bus != bus || clip.start > plan.seekTime) continue;
            const bool beforeEnd = plan.seekTime < clip.start + clip.duration;
            if (!beforeEnd && clip.stopAtEnd) continue;
            channel.playing = true;
            channel.clipId = clip.id;
            channel.path = clip.path;
            channel.loop = clip.loop;
            channel.volume = SampleClipVolume(clip, plan.seekTime);
            channel.offsetSeconds = std::max(0.0, plan.seekTime - clip.start);
        }
    };
    sampleAudio(PerformancePreviewAudioBus::Music, plan.audio.music);
    sampleAudio(PerformancePreviewAudioBus::Ambience, plan.audio.ambience);
    sampleAudio(PerformancePreviewAudioBus::Voice, plan.audio.voice);
    plan.audio.signature =
        std::string(plan.audio.music.managed ? "music:" : "") +
        plan.audio.music.clipId +
        (plan.audio.ambience.managed ? "|ambience:" : "") +
        plan.audio.ambience.clipId +
        (plan.audio.voice.managed ? "|voice:" : "") +
        plan.audio.voice.clipId;
    plan.unsafeEventsSkipped = static_cast<std::size_t>(std::ranges::count_if(
        events, [&](const auto& event) { return event.time <= plan.seekTime; }));
    return plan;
}

std::vector<PerformancePreviewEvent> PerformancePreviewSequence::EventsBetween(
    const double fromTime, const double toTime) const {
    std::vector<PerformancePreviewEvent> result;
    if (!std::isfinite(fromTime) || !std::isfinite(toTime) ||
        toTime < fromTime)
        return result;
    for (const auto& event : events) {
        const bool startsAtZero = fromTime == 0.0 && event.time == 0.0;
        if ((event.time > fromTime || startsAtZero) && event.time <= toTime)
            result.push_back(event);
    }
    return result;
}

Result<PerformancePreviewSequence> BuildPerformancePreviewSequence(
    const std::string_view performanceJson,
    const std::string_view projectManifestJson, const vn::GameCatalog& catalog,
    const std::string_view expectedSceneId,
    const PreviewResourceExists& exists) {
    const Json document = Json::parse(performanceJson, nullptr, false);
    const Json manifest = Json::parse(projectManifestJson, nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        document.value("format", std::string{}) != "PrismatiXPerformance" ||
        document.value("schemaRevision", 0U) != 2U ||
        document.value("sceneId", std::string{}) != expectedSceneId ||
        !document.contains("revision") ||
        !document["revision"].is_number_unsigned() ||
        !document.contains("stage") || !document["stage"].is_object() ||
        !document.contains("timeline") || !document["timeline"].is_object()) {
        return Result<PerformancePreviewSequence>::Failure(Problem(
            "PXWASM-PERFORMANCE-001",
            "Performance identity, schema, Stage, or Timeline is invalid."));
    }
    if (manifest.is_discarded() || !manifest.is_object() ||
        manifest.value("format", std::string{}) != "PrismatiXProject" ||
        !manifest.contains("assets") || !manifest["assets"].is_array()) {
        return Result<PerformancePreviewSequence>::Failure(Problem(
            "PXWASM-PERFORMANCE-002",
            "Performance Preview requires the active project asset manifest."));
    }
    const Json& timeline = document["timeline"];
    const double duration = NodeNumber(timeline, "duration", -1.0);
    if (duration <= 0.0 || !timeline.contains("tracks") ||
        !timeline["tracks"].is_array()) {
        return Result<PerformancePreviewSequence>::Failure(Problem(
            "PXWASM-PERFORMANCE-004",
            "Performance Timeline duration or tracks are invalid."));
    }
    const Json& stage = document["stage"];
    if (!stage.contains("nodes") || !stage["nodes"].is_array() ||
        stage["nodes"].size() > 65'536) {
        return Result<PerformancePreviewSequence>::Failure(Problem(
            "PXWASM-PERFORMANCE-005", "Performance Stage nodes are invalid."));
    }

    std::unordered_map<std::string, std::string> assets;
    for (const auto& asset : manifest["assets"]) {
        if (!asset.is_object() || !asset.contains("id") ||
            !asset["id"].is_string() || !asset.contains("source") ||
            !asset["source"].is_string())
            continue;
        assets[asset["id"].get<std::string>()] =
            asset["source"].get<std::string>();
    }

    PerformancePreviewSequence sequence;
    sequence.sceneId = std::string(expectedSceneId);
    if (const auto uiSceneId = stage.find("uiSceneId");
        uiSceneId != stage.end() && !uiSceneId->is_null()) {
        if (!uiSceneId->is_string() ||
            !Uuid::Parse(uiSceneId->get<std::string>())) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-020",
                "Performance Stage UI scene binding must be a canonical UUID or null.",
                {}, {}, "stage.uiSceneId"));
        }
        sequence.uiSceneId = uiSceneId->get<std::string>();
    }
    sequence.revision = document["revision"].get<std::uint64_t>();
    sequence.duration = duration;
    std::unordered_set<std::string> nodeIds;
    std::unordered_map<std::string, std::string> nodeKinds;
    for (const auto& node : stage["nodes"]) {
        if (!node.is_object()) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-006",
                "A Stage node has invalid identity or kind."));
        }
        const std::string id = node.value("id", std::string{});
        const std::string kind = node.value("kind", std::string{});
        if (id.empty() || !nodeIds.insert(id).second ||
            (kind != "background" && kind != "character" && kind != "ui" &&
             kind != "effect")) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-006",
                "A Stage node has invalid identity or kind.", {}, id));
        }
        nodeKinds.emplace(id, kind);
        const bool visible = node.value("visible", true);
        std::string imagePath;
        const std::string assetId = StringOrEmpty(node, "assetId");
        if (!assetId.empty()) {
            const auto found = assets.find(assetId);
            if (found != assets.end()) imagePath = found->second;
        }
        if (imagePath.empty() && kind == "character") {
            const std::string characterId =
                StringOrEmpty(node, "characterId");
            if (const auto image = catalog.ResolveCharacterImage(characterId);
                image && !image->lastKnownPath.empty())
                imagePath = image->lastKnownPath;
        }
        if (visible && (imagePath.empty() || (exists && !exists(imagePath)))) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-007",
                "A visible Stage node could not resolve its active image.", {},
                id, assetId.empty() ? "characterId" : "assetId"));
        }
        PerformancePreviewNode compiled;
        compiled.id = id;
        compiled.kind = kind;
        compiled.imagePath = std::move(imagePath);
        compiled.x = static_cast<float>(NodeNumber(node, "x", 0.0));
        compiled.y = static_cast<float>(NodeNumber(node, "y", 0.0));
        compiled.scaleX = static_cast<float>(NodeNumber(node, "scaleX", 1.0));
        compiled.scaleY = static_cast<float>(NodeNumber(node, "scaleY", 1.0));
        compiled.rotation = static_cast<float>(NodeNumber(node, "rotation", 0.0));
        compiled.opacity = static_cast<float>(NodeNumber(node, "opacity", 1.0));
        compiled.zOrder = node.value("zOrder", 0);
        compiled.visible = visible;
        if (!std::isfinite(compiled.x) || !std::isfinite(compiled.y) ||
            !std::isfinite(compiled.rotation) || compiled.scaleX <= 0.0f ||
            compiled.scaleY <= 0.0f || compiled.opacity < 0.0f ||
            compiled.opacity > 1.0f) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-008",
                "A Stage transform is outside its valid range.", {}, id));
        }
        sequence.baseNodes.push_back(std::move(compiled));
    }

    for (const auto& track : timeline["tracks"]) {
        if (!track.is_object() || !track.contains("kind") ||
            !track["kind"].is_string() || !track.contains("clips") ||
            !track["clips"].is_array() || !track.contains("keyframes") ||
            !track["keyframes"].is_array()) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-009", "A Timeline track is malformed."));
        }
        const std::string kind = track["kind"].get<std::string>();
        if (!KnownTrackKind(kind)) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-011",
                "Timeline track kind is not declared by Performance v2.", {},
                track.value("id", std::string{}), "kind"));
        }
        const bool muted = track.value("muted", false);
        std::string targetId;
        if (const auto target = track.find("targetId"); target != track.end()) {
            if (target->is_string()) targetId = target->get<std::string>();
            else if (!target->is_null()) {
                return Result<PerformancePreviewSequence>::Failure(Problem(
                    "PXWASM-PERFORMANCE-016",
                    "Timeline track target must be a Stage UUID or null.", {},
                    track.value("id", std::string{}), "targetId"));
            }
        }
        if (kind == "characterTransform" &&
            (targetId.empty() || !nodeIds.contains(targetId) ||
             nodeKinds[targetId] != "character")) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-012",
                "Character transform track target is missing from the active Stage.",
                {}, track.value("id", std::string{}), "targetId"));
        }
        if (kind == "character" &&
            (targetId.empty() || !nodeIds.contains(targetId) ||
             nodeKinds[targetId] != "character")) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-012",
                "Character clip track target is missing from the active Stage.",
                {}, track.value("id", std::string{}), "targetId"));
        }
        if ((kind == "camera" || kind == "effect" ||
             kind == "background" || kind == "audio" || kind == "voice" ||
             kind == "ui" || kind == "event") &&
            !targetId.empty()) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-016",
                "This global Timeline track cannot target a Stage node.",
                {}, track.value("id", std::string{}), "targetId"));
        }
        const bool numericKind = kind == "characterTransform" ||
                                 kind == "camera" || kind == "effect";
        const bool executableClipKind = kind == "background" ||
                                        kind == "character" ||
                                        kind == "audio" || kind == "voice" ||
                                        kind == "ui" || kind == "event";
        if (!track["keyframes"].empty() && !numericKind) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-014",
                "This Timeline track cannot execute numeric keyframes: " + kind,
                {}, track.value("id", std::string{}), "keyframes"));
        }
        if (!track["clips"].empty() && !executableClipKind) {
            return Result<PerformancePreviewSequence>::Failure(Problem(
                "PXWASM-PERFORMANCE-014",
                "This typed Timeline clip is not yet executable in the active Preview contract: " +
                    kind,
                {}, track.value("id", std::string{}), "clips"));
        }
        double previousClipStart = -1.0;
        for (const auto& clip : track["clips"]) {
            const std::string clipId = clip.value("id", std::string{});
            const double start = NodeNumber(clip, "start", -1.0);
            const double clipDuration = NodeNumber(clip, "duration", -1.0);
            if (!clip.is_object() || clipId.empty() || start < previousClipStart ||
                start < 0.0 || clipDuration <= 0.0 ||
                start + clipDuration > duration ||
                !clip.contains("payload") || !clip["payload"].is_object() ||
                clip["payload"].value("kind", std::string{}) != kind) {
                return Result<PerformancePreviewSequence>::Failure(Problem(
                    "PXWASM-PERFORMANCE-015",
                    "Timeline clip identity, range, order, or typed payload is invalid.",
                    {}, clipId, "payload"));
            }
            previousClipStart = start;
            const bool hasAssetId = !StringOrEmpty(clip, "assetId").empty();
            const auto assetPath = [&]() -> std::optional<std::string> {
                const std::string assetId = StringOrEmpty(clip, "assetId");
                if (assetId.empty()) return std::nullopt;
                const auto found = assets.find(assetId);
                if (found == assets.end() ||
                    (exists && !exists(found->second)))
                    return std::nullopt;
                return found->second;
            };
            const Json& payload = clip["payload"];
            if (kind == "character") {
                const std::string operation =
                    payload.value("operation", std::string{});
                const std::string transition =
                    payload.value("transition", std::string{});
                const int slot = payload.value("slot", 0);
                const bool hide = operation == "hide";
                const auto path = assetPath();
                if ((operation != "show" && operation != "expression" &&
                     !hide) ||
                    (transition != "cut" && transition != "crossfade") ||
                    slot < 1 || slot > 3 || (hide && hasAssetId) ||
                    (!hide && !path))
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-015",
                        "Character clip operation, asset, slot, or transition is invalid.",
                        {}, clipId, "payload"));
                if (!muted)
                    sequence.characterClips.push_back(
                        {clipId,
                         targetId,
                         start,
                         clipDuration,
                         hide ? PerformancePreviewCharacterOperation::Hide
                              : operation == "expression"
                                    ? PerformancePreviewCharacterOperation::Expression
                                    : PerformancePreviewCharacterOperation::Show,
                         path.value_or(std::string{}),
                         slot,
                         transition == "crossfade"});
                continue;
            }
            if (kind == "background") {
                const auto path = assetPath();
                const std::string transition =
                    payload.value("transition", std::string{});
                if (!path || (transition != "cut" && transition != "crossfade"))
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-015",
                        "Background clip asset or transition is invalid.", {},
                        clipId, "payload"));
                if (!muted)
                    sequence.backgroundClips.push_back(
                        {clipId, start, clipDuration, *path,
                         transition == "crossfade"});
                continue;
            }
            if (kind == "audio" || kind == "voice") {
                const auto path = assetPath();
                if (!path)
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-015",
                        "Audio or voice clip asset is missing from the Preview manifest.",
                        {}, clipId, "assetId"));
                PerformancePreviewAudioClip compiled;
                compiled.id = clipId;
                compiled.start = start;
                compiled.duration = clipDuration;
                compiled.path = *path;
                compiled.volume = payload.value("volume", -1);
                if (compiled.volume < 0 || compiled.volume > 128)
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-015",
                        "Timeline audio volume must be between 0 and 128.", {},
                        clipId, "payload.volume"));
                if (kind == "audio") {
                    const std::string bus = payload.value("bus", std::string{});
                    if (bus != "music" && bus != "ambience")
                        return Result<PerformancePreviewSequence>::Failure(Problem(
                            "PXWASM-PERFORMANCE-015",
                            "Timeline audio bus must be music or ambience.", {},
                            clipId, "payload.bus"));
                    compiled.bus = bus == "music"
                                       ? PerformancePreviewAudioBus::Music
                                       : PerformancePreviewAudioBus::Ambience;
                    compiled.loop = payload.value("loop", false);
                    compiled.fadeInMs = payload.value("fadeInMs", 0U);
                    compiled.fadeOutMs = payload.value("fadeOutMs", 0U);
                    if (compiled.bus == PerformancePreviewAudioBus::Music)
                        sequence.managesMusic = true;
                    else
                        sequence.managesAmbience = true;
                    if (compiled.fadeInMs > clipDuration * 1000.0 ||
                        compiled.fadeOutMs > clipDuration * 1000.0)
                        return Result<PerformancePreviewSequence>::Failure(Problem(
                            "PXWASM-PERFORMANCE-015",
                            "Timeline audio fades exceed the clip duration.", {},
                            clipId, "payload"));
                } else {
                    compiled.bus = PerformancePreviewAudioBus::Voice;
                    compiled.stopAtEnd = payload.value("stopAtEnd", true);
                    sequence.managesVoice = true;
                }
                if (!muted) {
                    if (std::ranges::any_of(
                            sequence.audioClips, [&](const auto& existing) {
                                return existing.bus == compiled.bus &&
                                       Overlaps(existing, compiled);
                            }))
                        return Result<PerformancePreviewSequence>::Failure(Problem(
                            "PXWASM-PERFORMANCE-017",
                            "Timeline audio clips overlap on the same typed bus.",
                            {}, clipId, "start"));
                    sequence.audioClips.push_back(std::move(compiled));
                }
                continue;
            }
            if (kind == "event") {
                const std::string actionId =
                    payload.value("actionId", std::string{});
                const auto arguments = payload.find("arguments");
                if ((clip.contains("assetId") && !clip["assetId"].is_null()) ||
                    actionId.empty() || arguments == payload.end() ||
                    !arguments->is_object() || arguments->dump().size() > 64 * 1024) {
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-018",
                        "Timeline event action or arguments are invalid.", {},
                        clipId, "payload"));
                }
                auto converted = VariantFromJson(*arguments);
                if (!converted || !converted->AsObject())
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-018",
                        "Timeline event arguments exceed the bounded Runtime value contract.",
                        {}, clipId, "payload.arguments"));
                if (!muted)
                    sequence.events.push_back(
                        {clipId, start, actionId, *converted->AsObject()});
                continue;
            }
            if (kind == "ui") {
                const std::string operation =
                    payload.value("operation", std::string{});
                const std::string uiTargetId =
                    payload.value("targetId", std::string{});
                const auto parsedTarget = Uuid::Parse(uiTargetId);
                if ((clip.contains("assetId") && !clip["assetId"].is_null()) ||
                    !parsedTarget ||
                    (operation != "show" && operation != "hide" &&
                     operation != "playAnimation"))
                    return Result<PerformancePreviewSequence>::Failure(Problem(
                        "PXWASM-PERFORMANCE-019",
                        "Timeline UI clip operation or target UUID is invalid.",
                        {}, clipId, "payload"));
                if (!muted)
                    sequence.uiClips.push_back(
                        {clipId,
                         start,
                         clipDuration,
                         operation == "hide"
                             ? PerformancePreviewUiOperation::Hide
                         : operation == "playAnimation"
                             ? PerformancePreviewUiOperation::PlayAnimation
                             : PerformancePreviewUiOperation::Show,
                         uiTargetId});
                if (!muted && operation == "playAnimation")
                    sequence.managesUiAnimation = true;
            }
        }
        std::unordered_map<std::string, PerformancePreviewNumericTrack> grouped;
        for (const auto& keyframe : track["keyframes"]) {
            const std::string property =
                keyframe.value("property", std::string{});
            const std::string curve = keyframe.value("curve", "linear");
            const bool propertyValid =
                kind == "characterTransform" ? TransformProperty(property)
                : kind == "camera" ? CameraProperty(property)
                                   : EffectProperty(property);
            if (!keyframe.is_object() || !propertyValid ||
                !FiniteNumber(keyframe, "time") ||
                !FiniteNumber(keyframe, "value") ||
                (curve != "linear" && curve != "easeInOut" &&
                 curve != "step" && curve != "hold")) {
                return Result<PerformancePreviewSequence>::Failure(Problem(
                    "PXWASM-PERFORMANCE-013",
                    "Timeline numeric keyframe is invalid for its typed track.", {},
                    keyframe.value("id", std::string{}), property));
            }
            const double time = keyframe["time"].get<double>();
            const double value = keyframe["value"].get<double>();
            if (time < 0.0 || time > duration ||
                ((property == "scaleX" || property == "scaleY") &&
                 value <= 0.0) ||
                (property == "opacity" && (value < 0.0 || value > 1.0)) ||
                (kind == "camera" && property == "zoom" && value <= 0.0) ||
                (kind == "effect" && (value < 0.0 || value > 1.0))) {
                return Result<PerformancePreviewSequence>::Failure(Problem(
                    "PXWASM-PERFORMANCE-013",
                    "Timeline numeric keyframe is outside its typed value range.", {},
                    keyframe.value("id", std::string{}), property));
            }
            if (muted) continue;
            auto& compiled = grouped[property];
            compiled.targetId = kind == "characterTransform" ? targetId : "$camera";
            compiled.property = property;
            compiled.fallback = GlobalPropertyFallback(kind, property);
            compiled.keyframes.push_back({time, value, curve});
        }
        for (auto& [property, compiled] : grouped) {
            (void)property;
            std::ranges::sort(compiled.keyframes, {},
                              &PerformancePreviewKeyframe::time);
            sequence.numericTracks.push_back(std::move(compiled));
        }
    }
    const auto startsByIdentity = [](const auto& left, const auto& right) {
        return left.start != right.start ? left.start < right.start
                                         : left.id < right.id;
    };
    std::ranges::sort(sequence.backgroundClips, startsByIdentity);
    std::ranges::sort(sequence.audioClips, startsByIdentity);
    std::ranges::sort(sequence.characterClips, startsByIdentity);
    std::ranges::sort(sequence.uiClips, startsByIdentity);
    if (!sequence.uiClips.empty() && sequence.uiSceneId.empty()) {
        return Result<PerformancePreviewSequence>::Failure(Problem(
            "PXWASM-PERFORMANCE-020",
            "Timeline UI clips require a bound Stage UI scene.", {}, {},
            "stage.uiSceneId"));
    }
    std::ranges::sort(sequence.events, [](const auto& left, const auto& right) {
        return left.time != right.time ? left.time < right.time
                                       : left.id < right.id;
    });
    return Result<PerformancePreviewSequence>::Success(std::move(sequence));
}

Result<PerformancePreviewPlan> BuildPerformancePreviewPlan(
    const std::string_view performanceJson,
    const std::string_view projectManifestJson, const vn::GameCatalog& catalog,
    const std::string_view expectedSceneId, const double requestedTime,
    const PreviewResourceExists& exists) {
    if (!std::isfinite(requestedTime) || requestedTime < 0.0) {
        return Result<PerformancePreviewPlan>::Failure(Problem(
            "PXWASM-PERFORMANCE-003", "Performance seek time is invalid."));
    }
    auto compiled = BuildPerformancePreviewSequence(
        performanceJson, projectManifestJson, catalog, expectedSceneId, exists);
    if (!compiled)
        return Result<PerformancePreviewPlan>::Failure(compiled.Diagnostics());
    return Result<PerformancePreviewPlan>::Success(
        compiled.Value().Sample(requestedTime));
}

}  // namespace px::preview
