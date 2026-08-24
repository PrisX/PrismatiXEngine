#include "Engine/Preview/PerformancePreview.h"

#include "Engine/VN/GameCatalog.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using Json = nlohmann::json;

std::string Manifest() {
    return Json{{"format", "PrismatiXProject"},
                {"schemaRevision", 1},
                {"assets",
                 Json::array({{{"id", "asset-bg"},
                               {"source", "Assets/background/night.png"},
                               {"kind", "background"}},
                              {{"id", "asset-music"},
                               {"source", "Assets/audio/theme.ogg"},
                               {"kind", "bgm"}},
                              {{"id", "asset-voice"},
                               {"source", "Assets/voice/line.ogg"},
                               {"kind", "voice"}},
                              {{"id", "asset-character"},
                               {"source", "Assets/character/rin.png"},
                               {"kind", "character"}}})}}
        .dump();
}

std::string Performance(const std::string& assetId = "asset-bg") {
    return Json{
        {"format", "PrismatiXPerformance"},
        {"schemaRevision", 2},
        {"sceneId", "scene-01"},
        {"revision", 4},
        {"stage",
         {{"safeArea", 0.9},
          {"gridSize", 16},
          {"snapEnabled", true},
          {"nodes",
           Json::array({{{"id", "node-bg"},
                         {"kind", "character"},
                         {"name", "Night"},
                         {"assetId", assetId},
                         {"characterId", nullptr},
                         {"x", 0},
                         {"y", 0},
                         {"scaleX", 1},
                         {"scaleY", 1},
                         {"rotation", 0},
                         {"opacity", 1},
                         {"zOrder", -10},
                         {"visible", true}}})}}},
        {"timeline",
         {{"duration", 2},
          {"frameRate", 60},
          {"tracks",
           Json::array({{{"id", "track-x"},
                         {"kind", "characterTransform"},
                         {"name", "Move"},
                         {"targetId", "node-bg"},
                         {"muted", false},
                         {"locked", false},
                         {"clips", Json::array()},
                         {"keyframes",
                          Json::array({{{"id", "key-a"},
                                        {"time", 0},
                                        {"property", "x"},
                                        {"value", 0},
                                        {"curve", "linear"}},
                                       {{"id", "key-b"},
                                        {"time", 2},
                                        {"property", "x"},
                                        {"value", 100},
                                        {"curve", "linear"}}})}}})},
          {"markers", Json::array()}}}}
        .dump();
}

}  // namespace

int main() {
    px::vn::GameCatalog catalog;
    auto planned = px::preview::BuildPerformancePreviewPlan(
        Performance(), Manifest(), catalog, "scene-01", 1.0,
        [](const std::string_view path) {
            return path == "Assets/background/night.png";
        });
    assert(planned);
    assert(planned.Value().revision == 4);
    assert(planned.Value().nodes.size() == 1);
    assert(std::abs(planned.Value().nodes.front().x - 50.0f) < 0.001f);
    assert(planned.Value().nodes.front().imagePath ==
           "Assets/background/night.png");

    auto compiled = px::preview::BuildPerformancePreviewSequence(
        Performance(), Manifest(), catalog, "scene-01",
        [](const std::string_view path) {
            return path == "Assets/background/night.png";
        });
    assert(compiled);
    assert(compiled.Value().duration == 2.0);
    assert(std::abs(compiled.Value().Sample(0.5).nodes.front().x - 25.0f) <
           0.001f);
    assert(std::abs(compiled.Value().Sample(1.5).nodes.front().x - 75.0f) <
           0.001f);

    Json globalTracks = Json::parse(Performance());
    globalTracks["timeline"]["tracks"].push_back(
        {{"id", "track-camera"},
         {"kind", "camera"},
         {"name", "Camera"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips", Json::array()},
         {"keyframes",
          Json::array({{{"id", "camera-a"},
                        {"time", 0},
                        {"property", "zoom"},
                        {"value", 1},
                        {"curve", "linear"}},
                       {{"id", "camera-b"},
                        {"time", 2},
                        {"property", "zoom"},
                        {"value", 2},
                        {"curve", "easeInOut"}}})}});
    globalTracks["timeline"]["tracks"].push_back(
        {{"id", "track-effect"},
         {"kind", "effect"},
         {"name", "Fade"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips", Json::array()},
         {"keyframes",
          Json::array({{{"id", "fade-a"},
                        {"time", 0},
                        {"property", "fade"},
                        {"value", 0},
                        {"curve", "linear"}},
                       {{"id", "fade-b"},
                        {"time", 2},
                        {"property", "fade"},
                        {"value", 1},
                        {"curve", "linear"}}})}});
    const auto compiledGlobals = px::preview::BuildPerformancePreviewSequence(
        globalTracks.dump(), Manifest(), catalog, "scene-01",
        [](const std::string_view path) {
            return path == "Assets/background/night.png";
        });
    assert(compiledGlobals);
    const auto globalFrame = compiledGlobals.Value().Sample(1.0);
    assert(globalFrame.properties.size() == 2);
    assert(globalFrame.properties[0].targetId == "$camera");
    assert(globalFrame.properties[0].property == "zoom");
    assert(std::abs(globalFrame.properties[0].value - 1.5) < 0.001);
    assert(globalFrame.properties[1].property == "fade");
    assert(std::abs(globalFrame.properties[1].value - 0.5) < 0.001);

    Json targetedCamera = globalTracks;
    targetedCamera["timeline"]["tracks"][1]["targetId"] = "node-bg";
    const auto invalidTarget = px::preview::BuildPerformancePreviewSequence(
        targetedCamera.dump(), Manifest(), catalog, "scene-01");
    assert(!invalidTarget);
    assert(invalidTarget.Diagnostics().front().code ==
           "PXWASM-PERFORMANCE-016");

    Json untypedClip = Json::parse(Performance());
    untypedClip["timeline"]["tracks"][0]["kind"] = "background";
    untypedClip["timeline"]["tracks"][0]["targetId"] = nullptr;
    untypedClip["timeline"]["tracks"][0]["keyframes"] = Json::array();
    untypedClip["timeline"]["tracks"][0]["clips"].push_back(
        {{"id", "clip-a"},
         {"name", "Missing payload"},
         {"start", 0},
         {"duration", 1},
         {"assetId", nullptr}});
    const auto rejectedClip = px::preview::BuildPerformancePreviewSequence(
        untypedClip.dump(), Manifest(), catalog, "scene-01");
    assert(!rejectedClip);
    assert(rejectedClip.Diagnostics().front().code ==
           "PXWASM-PERFORMANCE-015");

    Json typedClips = Json::parse(Performance());
    typedClips["stage"]["uiSceneId"] =
        "60606060-6060-4060-8060-606060606060";
    typedClips["stage"]["nodes"].push_back(
        {{"id", "node-character"},
         {"kind", "character"},
         {"name", "Rin"},
         {"assetId", "asset-character"},
         {"characterId", nullptr},
         {"x", 320},
         {"y", 360},
         {"scaleX", 1},
         {"scaleY", 1},
         {"rotation", 0},
         {"opacity", 1},
         {"zOrder", 1},
         {"visible", true}});
    typedClips["timeline"]["tracks"].push_back(
        {{"id", "track-background"},
         {"kind", "background"},
         {"name", "Background"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips",
          Json::array({{{"id", "clip-background"},
                        {"name", "Crossfade"},
                        {"start", 0.5},
                        {"duration", 0.5},
                        {"assetId", "asset-bg"},
                        {"payload",
                         {{"kind", "background"},
                          {"transition", "crossfade"}}}}})},
         {"keyframes", Json::array()}});
    typedClips["timeline"]["tracks"].push_back(
        {{"id", "track-character"},
         {"kind", "character"},
         {"name", "Character"},
         {"targetId", "node-character"},
         {"muted", false},
         {"locked", false},
         {"clips",
          Json::array({{{"id", "clip-character"},
                        {"name", "Show Rin"},
                        {"start", 0.5},
                        {"duration", 0.5},
                        {"assetId", "asset-character"},
                        {"payload",
                         {{"kind", "character"},
                          {"operation", "show"},
                          {"slot", 3},
                          {"transition", "crossfade"}}}}})},
         {"keyframes", Json::array()}});
    typedClips["timeline"]["tracks"].push_back(
        {{"id", "track-audio"},
         {"kind", "audio"},
         {"name", "Music"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips",
          Json::array({{{"id", "clip-music"},
                        {"name", "Theme"},
                        {"start", 0.25},
                        {"duration", 1.5},
                        {"assetId", "asset-music"},
                        {"payload",
                         {{"kind", "audio"},
                          {"bus", "music"},
                          {"loop", true},
                          {"volume", 96},
                          {"fadeInMs", 500},
                          {"fadeOutMs", 250}}}}})},
         {"keyframes", Json::array()}});
    typedClips["timeline"]["tracks"].push_back(
        {{"id", "track-voice"},
         {"kind", "voice"},
         {"name", "Voice"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips",
          Json::array({{{"id", "clip-voice"},
                        {"name", "Line"},
                        {"start", 0.0},
                        {"duration", 1.0},
                        {"assetId", "asset-voice"},
                        {"payload",
                         {{"kind", "voice"},
                          {"volume", 128},
                          {"stopAtEnd", true}}}}})},
         {"keyframes", Json::array()}});
    typedClips["timeline"]["tracks"].push_back(
        {{"id", "track-event"},
         {"kind", "event"},
         {"name", "Event"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips",
          Json::array({{{"id", "clip-event"},
                        {"name", "Flag"},
                        {"start", 1.0},
                        {"duration", 0.1},
                        {"assetId", nullptr},
                        {"payload",
                         {{"kind", "event"},
                          {"actionId", "flag.set"},
                          {"arguments", {{"name", "route-open"}}}}}}})},
         {"keyframes", Json::array()}});
    typedClips["timeline"]["tracks"].push_back(
        {{"id", "track-ui"},
         {"kind", "ui"},
         {"name", "HUD"},
         {"targetId", nullptr},
         {"muted", false},
         {"locked", false},
         {"clips",
          Json::array(
              {{{"id", "clip-ui-show"},
                {"name", "Show prompt"},
                {"start", 0.25},
                {"duration", 0.1},
                {"assetId", nullptr},
                {"payload",
                 {{"kind", "ui"},
                  {"operation", "show"},
                  {"targetId",
                   "11111111-1111-4111-8111-111111111111"}}}},
               {{"id", "clip-ui-animation"},
                {"name", "Prompt pulse"},
                {"start", 0.5},
                {"duration", 0.5},
                {"assetId", nullptr},
                {"payload",
                 {{"kind", "ui"},
                  {"operation", "playAnimation"},
                  {"targetId",
                   "22222222-2222-4222-8222-222222222222"}}}},
               {{"id", "clip-ui-hide"},
                {"name", "Hide prompt"},
                {"start", 1.25},
                {"duration", 0.1},
                {"assetId", nullptr},
                {"payload",
                 {{"kind", "ui"},
                  {"operation", "hide"},
                  {"targetId",
                   "11111111-1111-4111-8111-111111111111"}}}}})},
         {"keyframes", Json::array()}});
    const auto typedSequence = px::preview::BuildPerformancePreviewSequence(
        typedClips.dump(), Manifest(), catalog, "scene-01",
        [](const std::string_view path) {
            return path == "Assets/background/night.png" ||
                   path == "Assets/audio/theme.ogg" ||
                   path == "Assets/voice/line.ogg" ||
                   path == "Assets/character/rin.png";
        });
    assert(typedSequence);
    assert(typedSequence.Value().uiSceneId ==
           "60606060-6060-4060-8060-606060606060");
    const auto typedFrame = typedSequence.Value().Sample(0.75);
    assert(typedFrame.uiSceneId ==
           "60606060-6060-4060-8060-606060606060");
    assert(typedFrame.background);
    assert(typedFrame.background->crossfade);
    assert(typedFrame.characters.size() == 1);
    assert(typedFrame.characters.front().targetId == "node-character");
    assert(typedFrame.characters.front().slot == 3);
    assert(typedFrame.characters.front().crossfade);
    assert(std::ranges::none_of(typedFrame.nodes, [](const auto& node) {
        return node.id == "node-character";
    }));
    assert(typedFrame.audio.music.playing);
    assert(typedFrame.audio.music.clipId == "clip-music");
    assert(std::abs(typedFrame.audio.music.offsetSeconds - 0.5) < 0.001);
    assert(typedFrame.audio.voice.playing);
    assert(typedFrame.uiControls.size() == 1);
    assert(typedFrame.uiControls.front().visible);
    assert(typedFrame.uiAnimation);
    assert(typedFrame.uiAnimation->playing);
    assert(std::abs(typedFrame.uiAnimation->offsetSeconds - 0.25) < 0.001);
    const auto afterVoice = typedSequence.Value().Sample(1.25);
    assert(!afterVoice.audio.voice.playing);
    assert(afterVoice.uiControls.size() == 1);
    assert(!afterVoice.uiControls.front().visible);
    assert(afterVoice.uiAnimation);
    assert(!afterVoice.uiAnimation->playing);
    assert(afterVoice.unsafeEventsSkipped == 1);
    const auto events = typedSequence.Value().EventsBetween(0.5, 1.0);
    assert(events.size() == 1);
    assert(events.front().actionId == "flag.set");
    assert(events.front().arguments.contains("name"));

    Json unboundUi = typedClips;
    unboundUi["stage"]["uiSceneId"] = nullptr;
    const auto missingUiBinding =
        px::preview::BuildPerformancePreviewSequence(
            unboundUi.dump(), Manifest(), catalog, "scene-01");
    assert(!missingUiBinding);
    assert(missingUiBinding.Diagnostics().front().code ==
           "PXWASM-PERFORMANCE-020");

    Json unknown = Json::parse(Performance());
    unknown["timeline"]["tracks"][0]["kind"] = "browserApproximation";
    const auto unknownKind = px::preview::BuildPerformancePreviewSequence(
        unknown.dump(), Manifest(), catalog, "scene-01");
    assert(!unknownKind);
    assert(unknownKind.Diagnostics().front().code ==
           "PXWASM-PERFORMANCE-011");

    Json unsupported = Json::parse(Performance());
    unsupported["timeline"]["tracks"][0]["kind"] = "audio";
    unsupported["timeline"]["tracks"][0]["targetId"] = nullptr;
    const auto unsupportedKind = px::preview::BuildPerformancePreviewSequence(
        unsupported.dump(), Manifest(), catalog, "scene-01");
    assert(!unsupportedKind);
    assert(unsupportedKind.Diagnostics().front().code ==
           "PXWASM-PERFORMANCE-014");

    const auto missing = px::preview::BuildPerformancePreviewPlan(
        Performance("missing"), Manifest(), catalog, "scene-01", 0.0,
        [](const std::string_view) { return false; });
    assert(!missing);
    assert(!missing.Diagnostics().empty());
    assert(missing.Diagnostics().front().code == "PXWASM-PERFORMANCE-007");
    return 0;
}
