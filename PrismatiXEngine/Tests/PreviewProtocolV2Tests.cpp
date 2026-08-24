#include "Engine/Preview/PreviewProtocolV2.h"

#include <cassert>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using Json = nlohmann::json;

std::string RuntimeIr(const std::uint64_t revision,
                      const std::string& kind = "dialogue",
                      const Json& arguments =
                          Json{{"speaker", "雪"}, {"text", "晚安"}}) {
    return Json{{"format", "PrismatiXRuntimeIR"},
                {"schemaRevision", 1},
                {"documentId", "scene-01"},
                {"committedRevision", revision},
                {"operations",
                 Json::array({{{"operationId", "operation-01"},
                               {"sourceId", "block-01"},
                               {"sourceLine", 23},
                               {"kind", kind},
                               {"text", "@雪: 晚安"},
                               {"arguments", arguments}}})}}
        .dump();
}

std::string Envelope(const std::string& type, const std::uint64_t revision,
                     const std::string& runtimeIr,
                     const Json& performance = nullptr,
                     const Json& runtimeFiles = nullptr,
                     const Json& localization = nullptr) {
    Json payload{{"runtimeIr", runtimeIr}};
    if (!performance.is_null()) payload["performance"] = performance;
    if (!runtimeFiles.is_null()) payload["runtimeFiles"] = runtimeFiles;
    if (!localization.is_null()) payload["localization"] = localization;
    return Json{{"protocol", "PrismatiXPreviewProtocol"},
                {"schemaRevision", 2},
                {"protocolVersion", 2},
                {"type", type},
                {"sessionId", "session-01"},
                {"requestId", type + "-" + std::to_string(revision)},
                {"documentId", "scene-01"},
                {"revision", revision},
                {"payload", std::move(payload)}}
        .dump();
}

std::string ControlEnvelope(const std::uint64_t revision,
                            const Json& payload) {
    return Json{{"protocol", "PrismatiXPreviewProtocol"},
                {"schemaRevision", 2},
                {"protocolVersion", 2},
                {"type", "control"},
                {"sessionId", "session-01"},
                {"requestId", "control-" + std::to_string(revision)},
                {"documentId", "scene-01"},
                {"revision", revision},
                {"payload", payload}}
        .dump();
}

Json Performance(const std::uint64_t revision) {
    return Json{{"format", "PrismatiXPerformance"},
                {"schemaRevision", 1},
                {"sceneId", "scene-01"},
                {"revision", revision},
                {"stage", {{"nodes", Json::array()}}},
                {"timeline", {{"duration", 10.0}, {"tracks", Json::array()}}}};
}

Json RuntimeFiles() {
    return Json::array(
        {{{"virtualPath", "/project/Content/Extensions/game.pxextension"},
          {"sha256", std::string(64, 'a')},
          {"kind", "extensionManifest"},
          {"byteLength", 128}},
         {{"virtualPath", "/project/Content/Extensions/game.js"},
          {"sha256", std::string(64, 'b')},
          {"kind", "javascript"},
          {"byteLength", 256}}});
}

bool ContainsCode(const Json& events, const std::string& code) {
    for (const auto& event : events) {
        if (!event.contains("diagnostics") ||
            !event["diagnostics"].is_array())
            continue;
        for (const auto& diagnostic : event["diagnostics"])
            if (diagnostic.value("code", std::string{}) == code) return true;
    }
    return false;
}

}  // namespace

int main() {
    px::preview::PreviewProtocolV2 protocol;

    const auto applied = protocol.AcceptApply(
        Envelope("apply", 7, RuntimeIr(7), Performance(7), RuntimeFiles()),
        false);
    assert(applied.Accepted());
    assert(Json::parse(applied.request->performanceJson)
               .value("sceneId", std::string{}) == "scene-01");
    assert(applied.request->uiSceneJson.empty());
    assert(Json::parse(applied.request->uiComponentsJson).empty());
    assert(applied.request->localizationJson.empty());
    assert(Json::parse(applied.request->runtimeFilesJson) == RuntimeFiles());
    assert(protocol.SessionId().empty());
    assert(protocol.DocumentId().empty());
    assert(protocol.Revision() == 0);
    protocol.CommitApply(*applied.request);
    assert(protocol.SessionId() == "session-01");
    assert(protocol.DocumentId() == "scene-01");
    assert(protocol.Revision() == 7);

    auto events = Json::parse(protocol.DrainEvents());
    assert(events.is_array());
    assert(events.size() == 1);
    assert(events.front().value("type", std::string{}) == "state");
    assert(events.front().value("status", std::string{}) == "applying");

    px::preview::PreviewProtocolV2 malformedProtocol;
    const auto malformedRuntimeFiles = malformedProtocol.AcceptApply(
        Envelope("apply", 1, RuntimeIr(1), nullptr,
                 Json{{"virtualPath", "/project/Content/Extensions/game.js"}}),
        false);
    assert(!malformedRuntimeFiles.Accepted());
    assert(ContainsCode(Json::parse(malformedProtocol.DrainEvents()),
                        "PXWASM-PROTOCOL-008"));
    const auto incompleteRuntimeFile = malformedProtocol.AcceptApply(
        Envelope("apply", 1, RuntimeIr(1), nullptr,
                 Json::array({{{"virtualPath",
                                "/project/Content/Extensions/game.js"},
                               {"kind", "javascript"},
                               {"byteLength", 256}}})),
        false);
    assert(!incompleteRuntimeFile.Accepted());
    assert(ContainsCode(Json::parse(malformedProtocol.DrainEvents()),
                        "PXWASM-PROTOCOL-009"));
    const auto malformedLocalization = malformedProtocol.AcceptApply(
        Envelope("apply", 1, RuntimeIr(1), nullptr, nullptr,
                 Json::array()),
        false);
    assert(!malformedLocalization.Accepted());
    assert(ContainsCode(Json::parse(malformedProtocol.DrainEvents()),
                        "PXWASM-PROTOCOL-010"));
    const Json localization = {
        {"locale", "ja-JP"},
        {"pseudo", false},
        {"focusSourceId", "block-01"},
        {"document", {{"format", "PrismatiXLocalization"}}}};
    const auto localizedApply = malformedProtocol.AcceptApply(
        Envelope("apply", 1, RuntimeIr(1), nullptr, nullptr,
                 localization),
        false);
    assert(localizedApply.Accepted());
    assert(Json::parse(localizedApply.request->localizationJson) ==
           localization);

    const auto seek = protocol.AcceptControl(
        ControlEnvelope(7, {{"command", "seek"}, {"time", 2.5}}));
    assert(seek.Accepted());
    assert(seek.request->command == "seek");
    assert(Json::parse(seek.request->payloadJson).value("time", 0.0) ==
           2.5);

    const auto staleControl = protocol.AcceptControl(
        ControlEnvelope(6, {{"command", "pause"}}));
    assert(!staleControl.Accepted());
    assert(staleControl.resyncRequired);
    events = Json::parse(protocol.DrainEvents());
    assert(ContainsCode(events, "PXWASM-CONTROL-REVISION-001"));
    assert(events.back().value("status", std::string{}) ==
           "resyncRequired");

    const auto gap =
        protocol.AcceptApply(Envelope("patch", 9, RuntimeIr(9)), true);
    assert(!gap.Accepted());
    assert(gap.resyncRequired);
    events = Json::parse(protocol.DrainEvents());
    assert(ContainsCode(events, "PXWASM-REVISION-001"));
    assert(events.back().value("status", std::string{}) ==
           "resyncRequired");

    const auto patched =
        protocol.AcceptApply(Envelope("patch", 8, RuntimeIr(8)), true);
    assert(patched.Accepted());
    assert(protocol.Revision() == 7);
    protocol.CommitApply(*patched.request);
    assert(protocol.Revision() == 8);
    (void)protocol.DrainEvents();

    const auto video = protocol.AcceptApply(
        Envelope("apply", 9, RuntimeIr(9, "video", {{"file", "op.mp4"}})),
        false);
    assert(!video.Accepted());
    events = Json::parse(protocol.DrainEvents());
    assert(ContainsCode(events, "PXWASM-VIDEO-001"));
    assert(events.front().value("sessionId", std::string{}) == "session-01");
    assert(events.front().value("documentId", std::string{}) == "scene-01");
    assert(events.front().value("revision", 0) == 9);
    bool hasSource = false;
    for (const auto& event : events) {
        if (event.value("type", std::string{}) != "unsupported") continue;
        const auto& diagnostic = event["diagnostics"].front();
        hasSource = diagnostic["source"].value("blockId", std::string{}) ==
                        "block-01" &&
                    diagnostic.value("capability", std::string{}) ==
                        "video.ffmpeg" &&
                    diagnostic.value("nativeCheckAvailable", false);
    }
    assert(hasSource);

    const auto speech = protocol.AcceptApply(
        Envelope("apply", 9, RuntimeIr(9, "tts", {{"voice", "system"}})),
        false);
    assert(!speech.Accepted());
    assert(ContainsCode(Json::parse(protocol.DrainEvents()),
                        "PXWASM-SPEECH-001"));

    return 0;
}
