#include "Engine/Preview/PreviewProtocolV2.h"

#include "Engine/SDK/RuntimeIr.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace px::preview {
namespace {

using Json = nlohmann::json;

std::optional<std::string> RequiredString(const Json& value,
                                          const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string() || found->empty())
        return std::nullopt;
    return found->get<std::string>();
}

bool IsKind(const std::string_view value,
            const std::initializer_list<std::string_view> kinds) {
    return std::ranges::find(kinds, value) != kinds.end();
}

struct UnsupportedCapability {
    std::string_view code;
    std::string_view capability;
    std::string_view suggestion;
};

std::optional<UnsupportedCapability> Unsupported(
    const sdk::RuntimeIrOperation& operation) {
    std::string_view executableKind = operation.kind;
    if (operation.kind == "customNode") {
        const auto type = operation.arguments.find("type");
        if (type != operation.arguments.end()) executableKind = type->second;
    }
    if (IsKind(executableKind, {"video", "playVideo", "movie"})) {
        return UnsupportedCapability{
            "PXWASM-VIDEO-001", "video.ffmpeg",
            "Use Ship > Native Check to validate video playback."};
    }
    if (IsKind(executableKind,
               {"speech", "tts", "selfVoice", "platformSpeech"})) {
        return UnsupportedCapability{
            "PXWASM-SPEECH-001", "speech.platform",
            "Use Ship > Native Check to validate platform speech."};
    }
    if (IsKind(executableKind,
               {"nativeExtension", "extension.native"}) ||
        (operation.kind == "customNode" &&
         operation.arguments.contains("nativeLibrary"))) {
        return UnsupportedCapability{
            "PXWASM-EXTENSION-001", "extension.native",
            "Provide a portable RuntimeCore action or validate the native extension with Native Check."};
    }
    return std::nullopt;
}

Json Envelope(const std::string_view type, const std::string& sessionId,
              const std::string& requestId, const std::string& documentId,
              const std::uint64_t revision) {
    return {{"protocol", "PrismatiXPreviewProtocol"},
            {"schemaRevision", kSchemaRevision},
            {"protocolVersion", kProtocolVersion},
            {"type", type},
            {"sessionId", sessionId},
            {"requestId", requestId},
            {"documentId", documentId},
            {"revision", revision}};
}

}  // namespace

ApplyResult PreviewProtocolV2::AcceptApply(const std::string_view envelope,
                                           const bool patch) {
    ApplyResult result;
    const Json request = Json::parse(envelope, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        EmitError("PXWASM-PROTOCOL-001",
                  "Preview request is not valid JSON.");
        return result;
    }

    const auto sessionId = RequiredString(request, "sessionId");
    const auto requestId = RequiredString(request, "requestId");
    const auto documentId = RequiredString(request, "documentId");
    const auto protocol = RequiredString(request, "protocol");
    const auto type = RequiredString(request, "type");
    const auto revision = request.find("revision");
    const auto protocolVersion = request.find("protocolVersion");
    const auto schemaRevision = request.find("schemaRevision");
    const auto payload = request.find("payload");
    if (!sessionId || !requestId || !documentId || !protocol || !type ||
        revision == request.end() || !revision->is_number_unsigned() ||
        protocolVersion == request.end() ||
        !protocolVersion->is_number_unsigned() ||
        schemaRevision == request.end() ||
        !schemaRevision->is_number_unsigned() || payload == request.end() ||
        !payload->is_object()) {
        EmitError("PXWASM-PROTOCOL-002",
                  "Preview apply envelope is missing required identity fields.",
                  requestId.value_or(""));
        return result;
    }
    m_requestId = *requestId;
    if (*protocol != "PrismatiXPreviewProtocol" ||
        protocolVersion->get<std::uint32_t>() != kProtocolVersion ||
        schemaRevision->get<std::uint32_t>() != kSchemaRevision ||
        *type != (patch ? "patch" : "apply")) {
        EmitError("PXWASM-PROTOCOL-003",
                  "Preview protocol, schema, or request type is incompatible.",
                  *requestId);
        return result;
    }
    const auto runtimeIr = RequiredString(*payload, "runtimeIr");
    if (!runtimeIr) {
        EmitError("PXWASM-PROTOCOL-004",
                  "Preview apply payload requires UTF-8 runtimeIr.",
                  *requestId);
        return result;
    }
    std::string performanceJson;
    if (const auto performance = payload->find("performance");
        performance != payload->end() && !performance->is_null()) {
        if (!performance->is_object()) {
            EmitError("PXWASM-PROTOCOL-005",
                      "Preview performance payload must be an object or null.",
                      *requestId);
            return result;
        }
        performanceJson = performance->dump();
    }
    std::string uiSceneJson;
    if (const auto uiScene = payload->find("uiScene");
        uiScene != payload->end() && !uiScene->is_null()) {
        if (!uiScene->is_object()) {
            EmitError("PXWASM-PROTOCOL-006",
                      "Preview UI scene payload must be an object or null.",
                      *requestId);
            return result;
        }
        uiSceneJson = uiScene->dump();
    }
    std::string uiComponentsJson = "[]";
    if (const auto uiComponents = payload->find("uiComponents");
        uiComponents != payload->end()) {
        if (!uiComponents->is_array()) {
            EmitError("PXWASM-PROTOCOL-007",
                      "Preview UI components payload must be an array.",
                      *requestId);
            return result;
        }
        uiComponentsJson = uiComponents->dump();
    }
    std::string localizationJson;
    if (const auto localization = payload->find("localization");
        localization != payload->end() && !localization->is_null()) {
        if (!localization->is_object()) {
            EmitError("PXWASM-PROTOCOL-010",
                      "Preview localization payload must be an object or null.",
                      *requestId);
            return result;
        }
        localizationJson = localization->dump();
    }
    std::string runtimeFilesJson = "[]";
    if (const auto runtimeFiles = payload->find("runtimeFiles");
        runtimeFiles != payload->end()) {
        if (!runtimeFiles->is_array()) {
            EmitError("PXWASM-PROTOCOL-008",
                      "Preview Runtime files payload must be an array.",
                      *requestId);
            return result;
        }
        for (const auto& file : *runtimeFiles) {
            if (!file.is_object() ||
                !file.contains("virtualPath") ||
                !file["virtualPath"].is_string() ||
                !file.contains("sha256") || !file["sha256"].is_string() ||
                !file.contains("kind") || !file["kind"].is_string() ||
                !file.contains("byteLength") ||
                !file["byteLength"].is_number_unsigned()) {
                EmitError("PXWASM-PROTOCOL-009",
                          "Preview Runtime file entries require path, hash, kind, and byte length.",
                          *requestId);
                return result;
            }
        }
        runtimeFilesJson = runtimeFiles->dump();
    }

    const auto requestedRevision = revision->get<std::uint64_t>();
    m_correlationSessionId = *sessionId;
    m_correlationDocumentId = *documentId;
    m_correlationRevision = requestedRevision;
    if (patch) {
        const bool identityMismatch = m_sessionId.empty() ||
                                      *sessionId != m_sessionId ||
                                      *documentId != m_documentId;
        if (identityMismatch || requestedRevision != m_revision + 1) {
            result.resyncRequired = true;
            EmitError("PXWASM-REVISION-001",
                      "Preview patch revision has a gap; send a full apply.",
                      *requestId, true);
            return result;
        }
    }

    const auto parsed = sdk::ParseRuntimeIr(*runtimeIr);
    if (!parsed.Valid()) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : parsed.diagnostics) {
            diagnostics.push_back({{"severity", "error"},
                                   {"code", diagnostic.code},
                                   {"category", "Runtime.IR"},
                                   {"message", diagnostic.message},
                                   {"operationIndex",
                                    diagnostic.operationIndex}});
        }
        Emit("diagnostics", Json{{"diagnostics", diagnostics}}.dump());
        EmitError("PXWASM-RUNTIME-IR-001",
                  "Runtime IR was rejected by the shared contract.",
                  *requestId);
        return result;
    }
    if (parsed.document.documentId != *documentId ||
        parsed.document.committedRevision != requestedRevision) {
        EmitError("PXWASM-IDENTITY-001",
                  "Runtime IR identity or revision does not match the apply envelope.",
                  *requestId);
        return result;
    }

    Json unsupported = Json::array();
    for (const auto& operation : parsed.document.operations) {
        const auto capability = Unsupported(operation);
        if (!capability) continue;
        unsupported.push_back(
            {{"severity", "error"},
             {"code", capability->code},
             {"category", "Preview.UnsupportedCapability"},
             {"message", "This operation is unavailable in WASM Preview."},
             {"capability", capability->capability},
             {"nativeCheckAvailable", true},
             {"suggestion", capability->suggestion},
             {"source",
              {{"documentId", *documentId},
               {"blockId", operation.sourceId},
               {"operationId", operation.operationId},
               {"line", operation.sourceLine}}}});
    }
    if (!unsupported.empty()) {
        Emit("unsupported", Json{{"diagnostics", unsupported}}.dump());
        Emit("diagnostics", Json{{"diagnostics", unsupported}}.dump());
        return result;
    }

    result.request = ApplyRequest{*sessionId, *requestId, *documentId,
                                  requestedRevision, *runtimeIr,
                                  std::move(performanceJson),
                                  std::move(uiSceneJson),
                                  std::move(uiComponentsJson),
                                  std::move(localizationJson),
                                  std::move(runtimeFilesJson), patch};
    Emit("state", Json{{"status", "applying"},
                       {"mode", "wasm"},
                       {"patch", patch}}
                      .dump());
    return result;
}

ControlResult PreviewProtocolV2::AcceptControl(
    const std::string_view envelope) {
    ControlResult result;
    const Json request = Json::parse(envelope, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        EmitError("PXWASM-CONTROL-001",
                  "Preview control request is not valid JSON.");
        return result;
    }

    const auto sessionId = RequiredString(request, "sessionId");
    const auto requestId = RequiredString(request, "requestId");
    const auto documentId = RequiredString(request, "documentId");
    const auto protocol = RequiredString(request, "protocol");
    const auto type = RequiredString(request, "type");
    const auto revision = request.find("revision");
    const auto protocolVersion = request.find("protocolVersion");
    const auto schemaRevision = request.find("schemaRevision");
    const auto payload = request.find("payload");
    if (!sessionId || !requestId || !documentId || !protocol || !type ||
        revision == request.end() || !revision->is_number_unsigned() ||
        protocolVersion == request.end() ||
        !protocolVersion->is_number_unsigned() ||
        schemaRevision == request.end() ||
        !schemaRevision->is_number_unsigned() || payload == request.end() ||
        !payload->is_object()) {
        EmitError(
            "PXWASM-CONTROL-002",
            "Preview control envelope is missing required identity fields.",
            requestId.value_or(""));
        return result;
    }

    m_requestId = *requestId;
    const auto requestedRevision = revision->get<std::uint64_t>();
    m_correlationSessionId = *sessionId;
    m_correlationDocumentId = *documentId;
    m_correlationRevision = requestedRevision;
    if (*protocol != "PrismatiXPreviewProtocol" ||
        protocolVersion->get<std::uint32_t>() != kProtocolVersion ||
        schemaRevision->get<std::uint32_t>() != kSchemaRevision ||
        *type != "control") {
        EmitError("PXWASM-CONTROL-003",
                  "Preview control protocol, schema, or request type is incompatible.",
                  *requestId);
        return result;
    }
    const auto command = RequiredString(*payload, "command");
    if (!command) {
        EmitError("PXWASM-CONTROL-004",
                  "Preview control payload requires a command.", *requestId);
        return result;
    }
    if (m_sessionId.empty() || *sessionId != m_sessionId ||
        *documentId != m_documentId || requestedRevision != m_revision) {
        result.resyncRequired = true;
        EmitError(
            "PXWASM-CONTROL-REVISION-001",
            "Preview control identity does not match the applied Runtime revision; send a full apply.",
            *requestId, true);
        return result;
    }

    result.request = ControlRequest{*sessionId, *requestId, *documentId,
                                    requestedRevision, *command,
                                    payload->dump()};
    return result;
}

void PreviewProtocolV2::CommitApply(const ApplyRequest& request) {
    m_sessionId = request.sessionId;
    m_requestId = request.requestId;
    m_documentId = request.documentId;
    m_revision = request.revision;
    m_correlationSessionId = request.sessionId;
    m_correlationDocumentId = request.documentId;
    m_correlationRevision = request.revision;
}

void PreviewProtocolV2::Emit(const std::string_view type,
                             const std::string_view payloadJson) {
    Json event = Envelope(type,
                          m_correlationSessionId.empty()
                              ? m_sessionId
                              : m_correlationSessionId,
                          m_requestId,
                          m_correlationDocumentId.empty()
                              ? m_documentId
                              : m_correlationDocumentId,
                          m_correlationSessionId.empty()
                              ? m_revision
                              : m_correlationRevision);
    const Json payload = Json::parse(payloadJson, nullptr, false);
    if (!payload.is_discarded() && payload.is_object()) {
        for (auto entry = payload.begin(); entry != payload.end(); ++entry)
            event[entry.key()] = entry.value();
    }
    m_events.push_back(event.dump());
}

std::string PreviewProtocolV2::DrainEvents() {
    Json events = Json::array();
    for (const auto& event : m_events)
        events.push_back(Json::parse(event));
    m_events.clear();
    return events.dump();
}

void PreviewProtocolV2::Reset() {
    m_sessionId.clear();
    m_requestId.clear();
    m_documentId.clear();
    m_revision = 0;
    m_correlationSessionId.clear();
    m_correlationDocumentId.clear();
    m_correlationRevision = 0;
    m_events.clear();
}

void PreviewProtocolV2::EmitError(const std::string_view code,
                                  const std::string_view message,
                                  const std::string_view requestId,
                                  const bool resync) {
    if (!requestId.empty()) m_requestId = requestId;
    Emit("diagnostics",
         Json{{"diagnostics",
               Json::array({{{"severity", "error"},
                             {"code", code},
                             {"category", "Preview.Protocol"},
                             {"message", message}}})}}
             .dump());
    if (resync)
        Emit("state",
             Json{{"status", "resyncRequired"}, {"recoverable", true}}
                 .dump());
}

}  // namespace px::preview
