#pragma once

#include "Engine/SDK/ContractVersions.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::preview {

inline constexpr std::uint32_t kProtocolVersion =
    sdk::kPreviewProtocolVersion;
inline constexpr std::uint32_t kSchemaRevision = sdk::kPreviewSchemaRevision;

struct ApplyRequest {
    std::string sessionId;
    std::string requestId;
    std::string documentId;
    std::uint64_t revision = 0;
    std::string runtimeIr;
    std::string performanceJson;
    std::string uiSceneJson;
    std::string uiComponentsJson;
    std::string localizationJson;
    std::string runtimeFilesJson;
    bool patch = false;
};

struct ApplyResult {
    std::optional<ApplyRequest> request;
    bool resyncRequired = false;

    [[nodiscard]] bool Accepted() const { return request.has_value(); }
};

struct ControlRequest {
    std::string sessionId;
    std::string requestId;
    std::string documentId;
    std::uint64_t revision = 0;
    std::string command;
    std::string payloadJson;
};

struct ControlResult {
    std::optional<ControlRequest> request;
    bool resyncRequired = false;

    [[nodiscard]] bool Accepted() const { return request.has_value(); }
};

// Owns protocol identity, monotonic revisions, fail-closed capability checks,
// and event correlation. Rendering is deliberately outside this class so both
// native contract tests and the Emscripten composition exercise the same rules.
class PreviewProtocolV2 {
public:
    [[nodiscard]] ApplyResult AcceptApply(std::string_view envelope,
                                          bool patch);
    [[nodiscard]] ControlResult AcceptControl(std::string_view envelope);
    // AcceptApply validates and correlates a request without advancing the
    // accepted Runtime boundary. The composition calls CommitApply only after
    // RuntimeCore has installed the IR and every declared resource.
    void CommitApply(const ApplyRequest& request);
    void Emit(std::string_view type, std::string_view payloadJson = "{}");
    [[nodiscard]] std::string DrainEvents();
    void Reset();

    [[nodiscard]] const std::string& SessionId() const { return m_sessionId; }
    [[nodiscard]] const std::string& DocumentId() const { return m_documentId; }
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }

private:
    void EmitError(std::string_view code, std::string_view message,
                   std::string_view requestId = {}, bool resync = false);

    std::string m_sessionId;
    std::string m_requestId;
    std::string m_documentId;
    std::uint64_t m_revision = 0;
    std::string m_correlationSessionId;
    std::string m_correlationDocumentId;
    std::uint64_t m_correlationRevision = 0;
    std::vector<std::string> m_events;
};

}  // namespace px::preview
