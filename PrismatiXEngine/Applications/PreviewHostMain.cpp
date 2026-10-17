#include "Engine/Support/Utf8Path.h"
#include <SDL3/SDL.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Preview/PreviewProtocol.h"
#include "Engine/Preview/PreviewSessionFactory.h"
#include "Engine/Script/ScriptFrameScheduler.h"
#include "Engine/Script/ScriptHost.h"
#include "Engine/Script/VnModelSnapshots.h"
#include "Engine/Preview/PerformancePreview.h"
#include "Engine/Runtime.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/SDK/Ui.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/UI/Actions/BuiltInActionProvider.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/Widgets.h"
#include "Engine/VN/GameCatalog.h"
#include "Engine/Platform/ProcessWatch.h"

namespace {

using Json = nlohmann::json;
constexpr std::string_view kProtocol = "PrismatiXPreviewProtocol";
constexpr std::uint32_t kSchemaRevision = px::preview::kSchemaRevision;
constexpr std::uint32_t kProtocolVersion = px::preview::kProtocolVersion;
constexpr std::size_t kAsyncEventCapacity = 256;
constexpr std::size_t kRuntimeSnapshotItemLimit = 64;

struct RequestQueue {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Json> requests;
    bool inputClosed = false;
    // Set when the protocol client (parent process) is gone even though the
    // stdin pipe may still be open in some other process.
    bool parentExited = false;
    // Set once ReadRequests returned; until then the reader may be blocked
    // in a read that never completes.
    bool readerFinished = false;
};

Json Response(const Json& request, const std::string& type) {
    return Json{ { "protocol", kProtocol },
                 { "schemaRevision", kSchemaRevision },
                 { "protocolVersion", kProtocolVersion },
                 { "type", type },
                 { "sessionId", request.value("sessionId", std::string{}) },
                 { "requestId", request.value("requestId", std::string{}) },
                 { "documentId", request.value("documentId", std::string{}) },
                 { "revision", request.value("revision", 0ULL) } };
}

Json Error(const Json& request, const std::string& code, const std::string& message) {
    Json response = Response(request, "error");
    response["code"] = code;
    response["message"] = message;
    return response;
}

class AsyncEventChannel final {
public:
    void SetContext(const Json& request) {
        std::lock_guard lock(m_mutex);
        m_context =
            Json{ { "sessionId", request.value("sessionId", std::string{}) }, { "requestId", request.value("requestId", std::string{}) }, { "documentId", request.value("documentId", std::string{}) }, { "revision", request.value("revision", 0ULL) } };
    }

    void Push(const std::string_view type, Json payload) {
        std::lock_guard lock(m_mutex);
        if (m_events.size() == kAsyncEventCapacity) {
            m_events.pop_front();
            ++m_droppedEvents;
        }
        Json event = Response(m_context, std::string(type));
        event["async"] = true;
        event["eventSequence"] = ++m_eventSequence;
        event["droppedEventCount"] = m_droppedEvents;
        if (payload.is_object()) event.update(payload);
        m_events.push_back(std::move(event));
    }

    [[nodiscard]] std::deque<Json> Drain() {
        std::lock_guard lock(m_mutex);
        std::deque<Json> drained;
        drained.swap(m_events);
        return drained;
    }

private:
    std::mutex m_mutex;
    Json m_context{ { "sessionId", "" }, { "requestId", "" }, { "documentId", "" }, { "revision", 0 } };
    std::deque<Json> m_events;
    std::uint64_t m_eventSequence = 0;
    std::uint64_t m_droppedEvents = 0;
};

class RuntimeOutputSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit RuntimeOutputSink(std::shared_ptr<AsyncEventChannel> events) : m_events(std::move(events)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        std::string text(formatted.data(), formatted.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        const auto level = spdlog::level::to_string_view(message.level);
        m_events->Push("output", Json{ { "stream", "runtime" }, { "level", std::string(level.data(), level.size()) }, { "message", std::move(text) } });
    }

    void flush_() override {}

private:
    std::shared_ptr<AsyncEventChannel> m_events;
};

class SerialStdoutWriter final {
public:
    void Write(const Json& message) { std::cout << message.dump() << '\n' << std::flush; }
};

std::optional<std::string> RequiredString(const Json& request, const char* key) {
    const auto found = request.find(key);
    if (found == request.end() || !found->is_string() || found->empty()) return std::nullopt;
    return found->get<std::string>();
}

std::string StatusMessage(const px::Status& status, const std::string_view fallback) {
    if (status.Diagnostics().empty()) return std::string(fallback);
    const auto& diagnostic = status.Diagnostics().front();
    return diagnostic.code + ": " + diagnostic.message;
}

const char* PreviewSeverityName(
    const px::sdk::PreviewDiagnosticSeverity severity) {
    switch (severity) {
        case px::sdk::PreviewDiagnosticSeverity::Info:
            return "info";
        case px::sdk::PreviewDiagnosticSeverity::Warning:
            return "warning";
        case px::sdk::PreviewDiagnosticSeverity::Error:
            return "error";
        case px::sdk::PreviewDiagnosticSeverity::Fatal:
            return "fatal";
    }
    return "error";
}

Json PreviewDiagnosticJson(
    const px::sdk::PreviewSessionDiagnostic& diagnostic) {
    return {{"severity", PreviewSeverityName(diagnostic.severity)},
            {"code", diagnostic.code},
            {"category", diagnostic.category},
            {"message", diagnostic.message},
            {"details", diagnostic.details},
            {"source",
             {{"resourceId", diagnostic.source.resourceId},
              {"path", diagnostic.source.path},
              {"nodeId", diagnostic.source.nodeId},
              {"property", diagnostic.source.property},
              {"line", diagnostic.source.line},
              {"column", diagnostic.source.column},
              {"endLine", diagnostic.source.endLine},
              {"endColumn", diagnostic.source.endColumn}}},
            {"documentId", diagnostic.documentId},
            {"sourceId", diagnostic.sourceId},
            {"operationId", diagnostic.operationId},
            {"quickFix", diagnostic.quickFix},
            {"hint", diagnostic.hint},
            {"cause", diagnostic.cause},
            {"operationIndex", diagnostic.operationIndex}};
}

px::Status PreviewSessionFailure(std::string code, std::string message) {
    px::diag::Diagnostic diagnostic{
        .severity = px::diag::Severity::Error,
        .code = std::move(code),
        .category = "Preview.Session",
        .message = std::move(message)};
    return px::Status::Fail(std::move(diagnostic));
}

struct UiPreviewUpdatePlan {
    bool patch = false;
    std::size_t changedNodeCount = 0;
    std::string reason = "initialLoad";
};

struct RuntimeSourceLocation {
    std::string documentId;
    std::string uri;
};

Json WithoutKeys(Json value, const std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) return value;
    for (const auto key : keys) value.erase(std::string(key));
    return value;
}

UiPreviewUpdatePlan PlanUiPreviewUpdate(const Json& before, const Json& after) {
    if (!before.is_object() || !after.is_object()) return { .reason = "invalidDocumentShape" };
    if (WithoutKeys(before, { "revision", "name", "nodes" }) != WithoutKeys(after, { "revision", "name", "nodes" })) return { .reason = "documentStructureOrRuntimeGraphChanged" };
    if (!before.contains("nodes") || !before["nodes"].is_array() || !after.contains("nodes") || !after["nodes"].is_array() || before["nodes"].size() != after["nodes"].size()) return { .reason = "nodeTopologyChanged" };

    std::size_t changedNodeCount = 0;
    for (std::size_t index = 0; index < before["nodes"].size(); ++index) {
        const auto& oldNode = before["nodes"][index];
        const auto& newNode = after["nodes"][index];
        if (!oldNode.is_object() || !newNode.is_object()) return { .reason = "invalidNodeShape" };
        if (oldNode != newNode) ++changedNodeCount;
        // Only ordinary authored layout, content, appearance, visibility and
        // TypeRegistry-backed property changes are safe in-place. Everything
        // that owns callbacks, bindings, components, or hierarchy reloads.
        if (WithoutKeys(oldNode,
                        {"name", "visible", "locked", "layout", "text",
                         "assetId", "appearance", "accessibilityLabel",
                         "accessibilityRole", "accessibilityDescription",
                         "accessibilityFocusOrder", "runtimeProperties"}) !=
            WithoutKeys(newNode,
                        {"name", "visible", "locked", "layout", "text",
                         "assetId", "appearance", "accessibilityLabel",
                         "accessibilityRole", "accessibilityDescription",
                         "accessibilityFocusOrder", "runtimeProperties"}))
            return { .reason = "nodeStructureOrInteractionChanged" };
    }
    return { .patch = true, .changedNodeCount = changedNodeCount, .reason = changedNodeCount == 0 ? "revisionOnly" : "propertyPatch" };
}

UiPreviewUpdatePlan PlanPerformancePreviewUpdate(const Json& before, const Json& after) {
    if (!before.is_object() || !after.is_object()) return { .reason = "invalidDocumentShape" };
    if (WithoutKeys(before, { "revision", "stage", "timeline" }) != WithoutKeys(after, { "revision", "stage", "timeline" })) return { .reason = "performanceIdentityChanged" };
    if (!before.contains("stage") || !before["stage"].is_object() || !after.contains("stage") || !after["stage"].is_object() || !before["stage"].contains("nodes") ||
        !before["stage"]["nodes"].is_array() || !after["stage"].contains("nodes") || !after["stage"]["nodes"].is_array() ||
        before["stage"]["nodes"].size() != after["stage"]["nodes"].size())
        return { .reason = "stageTopologyChanged" };

    std::size_t changedNodeCount = 0;
    for (std::size_t index = 0; index < before["stage"]["nodes"].size(); ++index) {
        const auto& oldNode = before["stage"]["nodes"][index];
        const auto& newNode = after["stage"]["nodes"][index];
        if (!oldNode.is_object() || !newNode.is_object()) return { .reason = "invalidStageNodeShape" };
        if (oldNode != newNode) ++changedNodeCount;
        if (WithoutKeys(oldNode, { "name", "x", "y", "scaleX", "scaleY", "rotation", "anchorX", "anchorY", "opacity", "zOrder" }) !=
            WithoutKeys(newNode, { "name", "x", "y", "scaleX", "scaleY", "rotation", "anchorX", "anchorY", "opacity", "zOrder" }))
            return { .reason = "stageStructureAssetOrVisibilityChanged" };
    }
    return { .patch = true,
             .changedNodeCount = changedNodeCount,
             .reason = changedNodeCount == 0 ? "timelineOrSeekPatch" : "stagePropertyPatch" };
}

Json UiDiagnostic(
    const std::string_view severity,
    const std::string_view code,
    const std::string_view category,
    const std::string_view message,
    const std::string_view details,
    const std::string_view resourceId,
    const std::string_view path,
    const std::string_view nodeId = {},
    const std::string_view property = {},
    const int line = 0,
    const int column = 0
) {
    return { { "severity", severity }, { "code", code },       { "category", category },
             { "message", message },   { "details", details }, { "source", { { "resourceId", resourceId }, { "path", path }, { "nodeId", nodeId }, { "property", property }, { "line", line }, { "column", column } } } };
}

std::string_view VMStateName(const px::vn::VMState state) {
    switch (state) {
        case px::vn::VMState::Idle:
            return "idle";
        case px::vn::VMState::Running:
            return "running";
        case px::vn::VMState::WaitingClick:
            return "waitingClick";
        case px::vn::VMState::WaitingChoice:
            return "waitingChoice";
        case px::vn::VMState::WaitingTimer:
            return "waitingTimer";
        case px::vn::VMState::WaitingVideo:
            return "waitingVideo";
        case px::vn::VMState::WaitingExternal:
            return "waitingExternal";
        case px::vn::VMState::Paused:
            return "paused";
        case px::vn::VMState::Finished:
            return "finished";
    }
    return "unknown";
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code error;
    const auto relative = std::filesystem::relative(candidate, root, error);
    if (error || relative.empty() || relative.is_absolute()) return false;
    return *relative.begin() != "..";
}

std::optional<std::string> SafeProjectRelativePath(const std::string& value) {
    const std::filesystem::path path = px::PathFromUtf8(value);
    if (path.empty() || path.is_absolute()) return std::nullopt;
    for (const auto& part : path) {
        if (part == "..") return std::nullopt;
    }
    return px::PathToUtf8(path);
}

bool IsProjectRegularFile(const std::filesystem::path& root, const std::string& relativePath) {
    std::error_code error;
    const auto candidate = std::filesystem::canonical(root / px::PathFromUtf8(relativePath), error);
    return !error && IsWithin(root, candidate) && std::filesystem::is_regular_file(candidate, error) && !error;
}

std::optional<std::pair<std::filesystem::path, std::string>> ReadProjectFile(const std::string& projectRoot, const std::string& relativePath) {
    std::error_code error;
    const auto root = std::filesystem::canonical(px::PathFromUtf8(projectRoot), error);
    if (error) return std::nullopt;
    const std::filesystem::path requested = px::PathFromUtf8(relativePath);
    if (requested.is_absolute()) return std::nullopt;
    const auto file = std::filesystem::canonical(root / requested, error);
    if (error || !IsWithin(root, file)) return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream content;
    content << input.rdbuf();
    return std::pair{ root, content.str() };
}

std::optional<px::Variant> JsonVariant(const Json& value, const int depth = 0) {
    if (depth > 32) return std::nullopt;
    if (value.is_null()) return px::Variant{};
    if (value.is_boolean()) return px::Variant(value.get<bool>());
    if (value.is_number_integer()) return px::Variant(value.get<std::int64_t>());
    if (value.is_number()) return px::Variant(value.get<double>());
    if (value.is_string()) return px::Variant(value.get<std::string>());
    if (value.is_array()) {
        px::VariantArray result;
        for (const auto& item : value) {
            auto converted = JsonVariant(item, depth + 1);
            if (!converted) return std::nullopt;
            result.push_back(std::move(*converted));
        }
        return px::Variant(std::move(result));
    }
    if (value.is_object()) {
        px::VariantObject result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            auto converted = JsonVariant(item.value(), depth + 1);
            if (!converted) return std::nullopt;
            result.emplace(item.key(), std::move(*converted));
        }
        return px::Variant(std::move(result));
    }
    return std::nullopt;
}

px::ui::DialoguePresentation DialogueView(const px::RuntimeSession& session, const std::vector<std::string>& choices) {
    const auto& dialogue = session.Dialogue().State();
    px::ui::DialoguePresentation view;
    view.speaker = dialogue.speaker;
    view.text = dialogue.displayText;
    view.chapterTitle = session.VM().Chapter();
    view.musicTitle = session.VM().CurrentBgm();
    view.choices = choices;
    view.effect = dialogue.effect;
    view.effectProgress = dialogue.effectProgress;
    view.waiting = dialogue.finished && !dialogue.fullText.empty() && choices.empty();
    return view;
}

class PreviewHost final {
public:
    PreviewHost(RequestQueue& queue, std::shared_ptr<AsyncEventChannel> events) : m_queue(queue), m_events(std::move(events)) {
        (void)m_uiPreviewViewModel.Define("dialogue.speaker", px::Variant(std::string{ "Preview Speaker" }), true);
        (void)m_uiPreviewViewModel.Define("dialogue.text", px::Variant(std::string{ "Preview dialogue" }), true);
        (void)m_uiPreviewViewModel.Define("chapter.title", px::Variant(std::string{ "Preview Chapter" }), true);
        (void)m_uiPreviewViewModel.Define("music.title", px::Variant(std::string{ "Preview Music" }), true);
    }

    int Run() {
        px::diag::Global().SetListener([events = m_events](const px::diag::Diagnostic& diagnostic) {
            Json source{ { "resourceId", diagnostic.source.resourceId }, { "path", diagnostic.source.path }, { "nodeId", diagnostic.source.nodeId },
                         { "property", diagnostic.source.property },     { "line", diagnostic.source.line }, { "column", diagnostic.source.column } };
            events->Push(
                "diagnostics",
                Json{ { "diagnostics",
                        Json::array(
                            { Json{ { "severity", px::diag::ToString(diagnostic.severity) },
                                    { "code", diagnostic.code },
                                    { "category", diagnostic.category },
                                    { "message", diagnostic.message },
                                    { "details", diagnostic.details },
                                    { "source", std::move(source) } } }
                        ) } }
            );
        });
        while (!m_shutdown) {
            ProcessRequests();
            if (m_shutdown) break;
            {
                std::lock_guard lock(m_queue.mutex);
                if (m_queue.inputClosed && m_queue.requests.empty()) {
                    QueueExited(m_queue.parentExited ? "parentExited" : "inputClosed", true);
                    DrainEvents();
                    break;
                }
            }
            if (!m_runtime) {
                std::unique_lock lock(m_queue.mutex);
                m_queue.condition.wait(lock, [this] { return !m_queue.requests.empty() || m_queue.inputClosed; });
                if (m_queue.inputClosed && m_queue.requests.empty()) {
                    const bool parentExited = m_queue.parentExited;
                    lock.unlock();
                    QueueExited(parentExited ? "parentExited" : "inputClosed", true);
                    DrainEvents();
                    break;
                }
                continue;
            }
            if (!m_runtime->BeginFrame()) {
                QueueExited("runtimeWindowClosed", false);
                DrainEvents();
                break;
            }
            Frame();
            m_runtime->EndFrame();
            EmitRuntimeEventsIfChanged("frame");
            DrainEvents();
            SDL_Delay(1);
        }
        px::diag::Global().SetListener({});
        (void)m_uiPreview.Actions().UnregisterProvider("script");
        m_scriptHost.reset();
        m_previewSession.reset();
        m_session.reset();
        m_runtime.reset();
        return 0;
    }

private:
    struct ScriptCheckpoint {
        px::script::PendingCommandsState commands;
        px::script::PendingActionsState actions;
        px::script::ExtensionStates extensions;
        std::vector<px::diag::Diagnostic> captureErrors;
    };

    std::optional<px::sdk::PreviewSafety> InspectOperationSafety(
        const px::vn::Command& command) {
        if (command.type != "action") return std::nullopt;
        const Json payload = Json::parse(command.Get("value"), nullptr, false);
        if (payload.is_discarded() || !payload.is_object() ||
            !payload.contains("id") || !payload["id"].is_string())
            return px::sdk::PreviewSafety{};
        const auto* descriptor = m_uiPreview.Actions().Catalog().Find(
            payload["id"].get<std::string>());
        if (!descriptor) return px::sdk::PreviewSafety{};
        return px::sdk::PreviewSafety{
            descriptor->previewSafe, descriptor->deterministic,
            descriptor->seekSafe, descriptor->rollbackSafe};
    }

    std::shared_ptr<const void> CaptureExternalCheckpoint() const {
        if (!m_scriptHost) return {};
        auto checkpoint = std::make_shared<ScriptCheckpoint>();
        checkpoint->commands = m_scriptHost->CapturePending();
        checkpoint->actions = m_scriptHost->CapturePendingActions();
        auto extensions = m_scriptHost->CaptureExtensionState();
        if (extensions) checkpoint->extensions = extensions.TakeValue();
        else {
            checkpoint->captureErrors = extensions.Diagnostics();
            for (const auto& diagnostic : checkpoint->captureErrors)
                px::diag::Emit(diagnostic);
        }
        return checkpoint;
    }

    px::Status RestoreExternalCheckpoint(
        const std::shared_ptr<const void>& opaque) {
        if (!opaque) {
            if (m_scriptHost) m_scriptHost->CancelPending();
            return px::Status::Ok();
        }
        if (!m_scriptHost)
            return PreviewSessionFailure(
                "PXPREVIEW-NATIVE-SCRIPT-001",
                "JavaScript checkpoint cannot be restored without the script host.");
        const auto checkpoint =
            std::static_pointer_cast<const ScriptCheckpoint>(opaque);
        if (!checkpoint->captureErrors.empty())
            return px::Status::Fail(checkpoint->captureErrors);
        if (px::Status status = m_scriptHost->RestoreCheckpoint(
                checkpoint->commands, checkpoint->actions); !status)
            return status;
        return m_scriptHost->RestoreExtensionState(checkpoint->extensions);
    }

    template <typename PreviewResult>
    void WritePreviewFailure(const Json& request, const std::string& code,
                             const std::string& message,
                             const PreviewResult& result) {
        Json response = Error(request, code, message);
        response["previewStatus"] = static_cast<int>(result.status);
        response["diagnostics"] = Json::array();
        for (const auto& diagnostic : result.diagnostics) {
            response["diagnostics"].push_back(PreviewDiagnosticJson(diagnostic));
        }
        Write(response);
        if (m_previewSession) (void)m_previewSession->Events();
    }

    void ProcessRequests() {
        std::optional<Json> pending;
        {
            std::lock_guard lock(m_queue.mutex);
            if (!m_queue.requests.empty()) {
                pending = std::move(m_queue.requests.front());
                m_queue.requests.pop_front();
            }
        }
        if (!pending) return;
        Handle(*pending);
        if (m_previewSession) (void)m_previewSession->Events();
        EmitRuntimeEventsIfChanged("request");
        DrainEvents();
    }

    void Handle(const Json& request) {
        m_events->SetContext(request);
        const std::string requestId = request.value("requestId", std::string{});
        if (request.value("protocol", std::string{}) != kProtocol || request.value("schemaRevision", 0U) != kSchemaRevision || request.value("protocolVersion", 0U) != kProtocolVersion) {
            Write(Error(request, "protocol-mismatch", "Unsupported preview protocol contract"));
            return;
        }
        if (requestId.empty() || request.value("sessionId", std::string{}).empty() || !request.contains("documentId") || !request["documentId"].is_string() || !request.contains("revision") || !request["revision"].is_number_unsigned()) {
            Write(Error(request, "invalid-envelope", "sessionId, requestId, documentId and revision are required"));
            return;
        }
        const auto type = RequiredString(request, "type");
        if (!type) {
            Write(Error(request, "missing-type", "Preview request type is required"));
            return;
        }
        if (*type == "hello") {
            Json response = Response(request, "ready");
            response["runtimeIrSchemaRevision"] = 1;
            response["uiSchemaRevision"] = 1;
            response["renderMode"] = "nativeWindow";
            Write(response);
            spdlog::info("PreviewHost protocol session is ready");
            m_events->Push("state", Json{ { "scope", "host" }, { "state", "ready" }, { "reason", "handshakeCompleted" } });
            m_events->Push("debug", Json{ { "scope", "debugger" }, { "state", "detached" }, { "paused", false }, { "reason", "runtimeNotLoaded" } });
        }
        else if (*type == "shutdown") {
            Write(Response(request, "shutdownAccepted"));
            QueueExited("shutdownRequested", true);
            m_shutdown = true;
        }
        else if (*type == "applyRuntimeIr") {
            Apply(request, false);
        }
        else if (*type == "patchRuntimeIr") {
            Apply(request, true);
        }
        else if (*type == "applyUiScene") {
            ApplyUiScene(request);
        }
        else if (*type == "activateUiControl") {
            ActivateUiControl(request);
        }
        else if (*type == "seekPerformance") {
            SeekPerformance(request);
        }
        else if (*type == "seekStory") {
            if (!m_previewSession) {
                Write(Error(request, "runtime-not-loaded", "Load a document before seeking Story Preview"));
                return;
            }
            const auto operation = request.find("operationIndex");
            if (operation == request.end() || !operation->is_number_integer()) {
                Write(Error(request, "invalid-story-seek", "seekStory requires an integer operationIndex"));
                return;
            }
            std::vector<int> branchPath;
            if (const auto encoded = request.find("branchPath");
                encoded != request.end()) {
                if (!encoded->is_array()) {
                    Write(Error(request, "invalid-story-seek", "seekStory branchPath must be an array"));
                    return;
                }
                for (const auto& choice : *encoded) {
                    if (!choice.is_number_integer() || choice.get<int>() < 0) {
                        Write(Error(request, "invalid-story-seek", "seekStory branchPath must contain non-negative choice indices"));
                        return;
                    }
                    branchPath.push_back(choice.get<int>());
                }
            }
            const auto sought = m_previewSession->SeekStory(
                {operation->get<int>(), std::move(branchPath)});
            if (!sought.accepted) {
                WritePreviewFailure(request, "story-seek-rejected",
                                    "Story seek could not be completed.", sought);
                return;
            }
            Json response = RuntimeStateResponse(request, "storySeekAccepted");
            response["operationIndex"] = operation->get<int>();
            Write(response);
        }
        else if (*type == "applyTimeline") {
            const auto projectRoot = RequiredString(request, "projectRoot");
            const auto documentId = RequiredString(request, "documentId");
            const auto timeline = RequiredString(request, "timeline");
            if (!projectRoot || !documentId || !timeline) {
                Write(Error(request, "invalid-timeline-apply",
                            "applyTimeline requires projectRoot, documentId, and canonical timeline text"));
                return;
            }
            if (!EnsureRuntime(*projectRoot)) {
                Write(Error(request,
                            m_runtimeStartupErrorCode.empty()
                                ? "runtime-start-failed"
                                : m_runtimeStartupErrorCode,
                            m_runtimeStartupErrorMessage.empty()
                                ? "Native preview runtime could not start"
                                : m_runtimeStartupErrorMessage));
                return;
            }
            const double start = request.value("time", 0.0);
            const double speed = request.value("speed", 1.0);
            const std::string sourcePath = request.value(
                "sourcePath", "memory://preview/timeline.pxtimeline");
            const auto applied = m_previewSession->ApplyTimeline(
                {*documentId, request.value("revision", std::uint64_t{0}),
                 *timeline, sourcePath, start, speed});
            if (!applied.accepted) {
                WritePreviewFailure(request, "timeline-apply-rejected",
                                    "Runtime Timeline could not be applied.",
                                    applied);
                return;
            }
            Json response = RuntimeStateResponse(request, "timelineApplied");
            response["playbackHandle"] = applied.playbackHandle;
            response["time"] = start;
            Write(response);
        }
        else if (*type == "seekTimeline") {
            if (!m_previewSession) {
                Write(Error(request, "runtime-not-loaded", "Load a document before seeking a Runtime Timeline"));
                return;
            }
            const auto handle = request.find("playbackHandle");
            const auto seconds = request.find("time");
            if (handle == request.end() || !handle->is_number_unsigned() ||
                seconds == request.end() || !seconds->is_number()) {
                Write(Error(request, "invalid-timeline-seek", "seekTimeline requires playbackHandle and time"));
                return;
            }
            const auto sought = m_previewSession->SeekTimeline(
                {handle->get<std::uint64_t>(), seconds->get<double>()});
            if (!sought.accepted) {
                WritePreviewFailure(request, "timeline-seek-rejected",
                                    "Runtime Timeline seek could not be completed.",
                                    sought);
                return;
            }
            Json response = RuntimeStateResponse(request, "timelineSeekAccepted");
            response["playbackHandle"] = handle->get<std::uint64_t>();
            response["time"] = seconds->get<double>();
            Write(response);
        }
        else if (*type == "resize") {
            if (!m_previewSession) {
                Write(Error(request, "runtime-not-loaded", "Load a document before resizing Preview"));
                return;
            }
            const auto width = request.find("width");
            const auto height = request.find("height");
            const double scale = request.value("scale", 1.0);
            if (width == request.end() || !width->is_number_integer() ||
                height == request.end() || !height->is_number_integer()) {
                Write(Error(request, "invalid-preview-size", "resize requires integer width and height"));
                return;
            }
            const auto resized = m_previewSession->Resize(
                width->get<int>(), height->get<int>(),
                static_cast<float>(scale));
            if (!resized.accepted) {
                WritePreviewFailure(request, "preview-resize-rejected",
                                    "Preview could not be resized.", resized);
                return;
            }
            Write(RuntimeStateResponse(request, "resizeAccepted"));
        }
        else if (*type == "advance") {
            if (!m_session) {
                Write(Error(request, "runtime-not-loaded", "Load a document before advancing Preview"));
                return;
            }
            const auto state = m_session->VM().State();
            if (state != px::vn::VMState::WaitingClick && state != px::vn::VMState::WaitingTimer) {
                Write(Error(request, "preview-cannot-advance", "Advance requires a click- or timer-waiting Runtime"));
                return;
            }
            const auto advanced = m_previewSession->Advance();
            if (!advanced.accepted) {
                WritePreviewFailure(request, "preview-cannot-advance",
                                    "Preview could not advance.", advanced);
                return;
            }
            Write(RuntimeStateResponse(request, "advanceAccepted"));
        }
        else if (*type == "selectChoice") {
            if (!m_session || m_session->VM().State() != px::vn::VMState::WaitingChoice) {
                Write(Error(request, "preview-not-waiting-choice", "Choice selection requires a choice-waiting Runtime"));
                return;
            }
            const auto index = request.find("index");
            if (index == request.end() || !index->is_number_integer() || index->get<int>() < 0 ||
                static_cast<std::size_t>(index->get<int>()) >= m_session->VM().Choices().size()) {
                Write(Error(request, "invalid-choice-index", "Choice index must identify a current Runtime choice"));
                return;
            }
            const auto selected =
                m_previewSession->SelectChoice(index->get<int>());
            if (!selected.accepted) {
                WritePreviewFailure(request, "invalid-choice-index",
                                    "Preview could not select the choice.",
                                    selected);
                return;
            }
            Write(RuntimeStateResponse(request, "choiceAccepted"));
        }
        else if (*type == "setAudioLevels") {
            if (!m_session || !m_runtime) {
                Write(Error(request, "runtime-not-loaded", "Load a document before controlling Preview audio"));
                return;
            }
            const auto levels = request.find("levels");
            if (levels == request.end() || !levels->is_object() || levels->empty()) {
                Write(Error(request, "invalid-audio-levels", "setAudioLevels requires at least one named level"));
                return;
            }
            static const std::set<std::string> allowed = { "main", "music", "voice", "sfx", "ambience" };
            for (const auto& [name, value] : levels->items()) {
                if (!allowed.contains(name) || !value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 128) {
                    Write(Error(request, "invalid-audio-levels", "Audio levels must use known channels and integer values from 0 to 128"));
                    return;
                }
            }
            auto& audio = m_runtime->Audio();
            if (levels->contains("main")) audio.SetMainVolume((*levels)["main"].get<int>());
            if (levels->contains("music")) audio.SetBGMVolume((*levels)["music"].get<int>());
            if (levels->contains("voice")) audio.SetVoiceVolume((*levels)["voice"].get<int>());
            if (levels->contains("sfx")) audio.SetSEVolume((*levels)["sfx"].get<int>());
            if (levels->contains("ambience")) audio.SetAmbienceVolume((*levels)["ambience"].get<int>());
            const auto applied = audio.CaptureState();
            Json response = RuntimeStateResponse(request, "audioLevelsSet");
            response["audioLevels"] = { { "main", applied.mainVolume },
                                        { "music", applied.musicVolume },
                                        { "voice", applied.voiceVolume },
                                        { "sfx", applied.sfxVolume },
                                        { "ambience", applied.ambienceVolume } };
            Write(response);
        }
        else if (*type == "play" || *type == "continue") {
            if (!m_session) {
                Write(Error(request, "runtime-not-loaded", "Load a document before controlling Preview"));
                return;
            }
            const auto controlled = *type == "play"
                ? m_previewSession->Play()
                : m_previewSession->Continue();
            if (!controlled.accepted) {
                WritePreviewFailure(request, "preview-cannot-play",
                                    "Preview could not resume.", controlled);
                return;
            }
            if (*type == "play" && request.value("showTitle", false)) {
                (void)m_previewSession->Pause();
                const auto shown = m_hud.ShowTitle();
                if (!shown) {
                    Write(Error(request, "preview-title-rejected", StatusMessage(shown, "Title could not be shown")));
                    return;
                }
            }
            Write(RuntimeStateResponse(request, *type == "play" ? "playAccepted" : "continueAccepted"));
        }
        else if (*type == "pause") {
            if (!m_session) {
                Write(Error(request, "runtime-not-loaded", "Load a document before controlling Preview"));
                return;
            }
            const auto paused = m_previewSession->Pause();
            if (!paused.accepted) {
                WritePreviewFailure(request, "preview-cannot-pause",
                                    "Preview is already paused or has finished.",
                                    paused);
                return;
            }
            Write(RuntimeStateResponse(request, "pauseAccepted"));
        }
        else if (*type == "step") {
            if (!m_session || m_session->VM().State() != px::vn::VMState::Paused || m_session->VM().ManuallyPaused()) {
                Write(Error(request, "invalid-debug-state", "Step requires a breakpoint-paused runtime"));
                return;
            }
            m_session->VM().DebugStep();
            Write(RuntimeStateResponse(request, "stepAccepted"));
        }
        else if (*type == "capture") {
            if (!m_session) {
                Write(Error(request, "runtime-not-loaded", "Load a document before capturing Preview state"));
                return;
            }
            const auto checkpoint = m_previewSession->CaptureCheckpoint();
            if (!checkpoint.accepted || !checkpoint.checkpoint) {
                WritePreviewFailure(request, "capture-rejected",
                                    "Preview checkpoint could not be captured.",
                                    checkpoint);
                return;
            }
            Json response = RuntimeStateResponse(request, "stateCaptured");
            const auto state = m_session->CaptureState();
            response["variables"] = Json::object();
            for (const auto& [name, value] : state.variables) {
                response["variables"][name] = value;
            }
            response["callStack"] = Json::array();
            for (const auto& frame : state.vm.callStack) {
                response["callStack"].push_back({ { "script", frame.script }, { "programCounter", frame.pc } });
            }
            response["runtimeSnapshot"] = RuntimeSnapshotPayload(state);
            response["checkpointId"] = checkpoint.checkpoint->id;
            response["operationIndex"] = checkpoint.checkpoint->operationIndex;
            response["branchPath"] = checkpoint.checkpoint->branchPath;
            Write(response);
        }
        else if (*type == "restoreCheckpoint") {
            if (!m_previewSession) {
                Write(Error(request, "runtime-not-loaded", "Load a document before restoring Preview state"));
                return;
            }
            const auto checkpointId = request.find("checkpointId");
            if (checkpointId == request.end() ||
                !checkpointId->is_number_unsigned()) {
                Write(Error(request, "invalid-checkpoint", "restoreCheckpoint requires an unsigned checkpointId"));
                return;
            }
            const auto restored = m_previewSession->RestoreCheckpoint(
                checkpointId->get<std::uint64_t>());
            if (!restored.accepted) {
                WritePreviewFailure(request, "checkpoint-restore-rejected",
                                    "Preview checkpoint could not be restored.",
                                    restored);
                return;
            }
            Json response = RuntimeStateResponse(request, "checkpointRestored");
            response["checkpointId"] = checkpointId->get<std::uint64_t>();
            Write(response);
        }
        else if (*type == "setBreakpoints") {
            if (!m_session) {
                Write(Error(request, "runtime-not-loaded", "Load a document before setting breakpoints"));
                return;
            }
            const auto breakpoints = request.find("lines");
            if (breakpoints == request.end() || !breakpoints->is_array()) {
                Write(Error(request, "invalid-breakpoints", "setBreakpoints requires an array of positive source lines"));
                return;
            }
            std::set<int> availableLines;
            for (const auto& command : m_session->VM().CurrentProgram().code) {
                if (command.line > 0) availableLines.insert(command.line);
            }
            std::set<int> requestedLines;
            for (const auto& line : *breakpoints) {
                if (!line.is_number_integer() || line.get<int>() <= 0) {
                    Write(Error(request, "invalid-breakpoints", "breakpoint lines must be positive integers"));
                    return;
                }
                requestedLines.insert(line.get<int>());
            }
            m_session->VM().ClearBreakpoints();
            Json verified = Json::array();
            Json unresolved = Json::array();
            for (const int line : requestedLines) {
                if (availableLines.contains(line)) {
                    m_session->VM().ToggleBreakpoint(line);
                    verified.push_back(line);
                }
                else {
                    unresolved.push_back(line);
                }
            }
            Json response = RuntimeStateResponse(request, "breakpointsSet");
            response["breakpointLines"] = std::move(verified);
            response["unresolvedBreakpoints"] = std::move(unresolved);
            Write(response);
        }
        else if (*type == "setScriptBreakpoints") {
            if (!m_scriptHost || m_projectRoot.empty()) {
                Write(Error(request, "runtime-not-loaded", "Load a document before setting script breakpoints"));
                return;
            }
            const auto breakpoints = request.find("breakpoints");
            if (breakpoints == request.end() || !breakpoints->is_array()) {
                Write(Error(request, "invalid-script-breakpoints", "setScriptBreakpoints requires source and line objects"));
                return;
            }
            std::vector<px::script::DebugBreakpoint> verifiedBreakpoints;
            Json verified = Json::array();
            Json unresolved = Json::array();
            for (const auto& breakpoint : *breakpoints) {
                if (!breakpoint.is_object() || !breakpoint.contains("source") || !breakpoint["source"].is_string() || !breakpoint.contains("line") || !breakpoint["line"].is_number_integer()) {
                    Write(Error(request, "invalid-script-breakpoints", "Script breakpoints require a project-relative source and positive line"));
                    return;
                }
                const auto source = SafeProjectRelativePath(breakpoint["source"].get<std::string>());
                const int line = breakpoint["line"].get<int>();
                if (!source || line <= 0) {
                    Write(Error(request, "invalid-script-breakpoints", "Script breakpoint paths and lines must be safe and positive"));
                    return;
                }
                const auto loaded = ReadProjectFile(px::PathToUtf8(m_projectRoot), *source);
                std::size_t lineCount = 0;
                if (loaded) {
                    lineCount = 1 + static_cast<std::size_t>(std::count(loaded->second.begin(), loaded->second.end(), '\n'));
                }
                const Json item{ { "source", *source }, { "line", line } };
                if (loaded && static_cast<std::size_t>(line) <= lineCount) {
                    verifiedBreakpoints.push_back({ *source, line });
                    verified.push_back(item);
                }
                else {
                    unresolved.push_back(item);
                }
            }
            m_scriptBreakpoints = m_scriptHost->SetDebugBreakpoints(std::move(verifiedBreakpoints));
            Json response = RuntimeStateResponse(request, "scriptBreakpointsSet");
            response["scriptBreakpoints"] = std::move(verified);
            response["unresolvedScriptBreakpoints"] = std::move(unresolved);
            Write(response);
        }
        else if (*type == "scriptPause") {
            if (!m_scriptHost || !m_scriptHost->DebugPause()) {
                Write(Error(request, "script-cannot-pause", "Script execution is already paused or no debug session is available"));
                return;
            }
            Write(RuntimeStateResponse(request, "scriptPauseAccepted"));
        }
        else if (*type == "scriptContinue") {
            if (!m_scriptHost || !m_scriptHost->DebugContinue()) {
                Write(Error(request, "invalid-script-debug-state", "Continue requires paused script execution"));
                return;
            }
            Write(RuntimeStateResponse(request, "scriptContinueAccepted"));
        }
        else if (*type == "scriptStep") {
            if (!m_scriptHost || !m_scriptHost->DebugStep()) {
                Write(Error(request, "invalid-script-debug-state", "Step requires paused script execution"));
                return;
            }
            Write(RuntimeStateResponse(request, "scriptStepAccepted"));
        }
        else if (*type == "evaluateScriptWatches") {
            if (!m_scriptHost || !m_scriptHost->CaptureDebugState().paused) {
                Write(Error(request, "invalid-script-debug-state", "Watch evaluation requires paused script execution"));
                return;
            }
            const auto watches = request.find("watches");
            if (watches == request.end() || !watches->is_array()) {
                Write(Error(request, "invalid-script-watches", "evaluateScriptWatches requires an array of watch expressions"));
                return;
            }
            Json response = RuntimeStateResponse(request, "scriptWatchesEvaluated");
            response["scriptWatches"] = Json::array();
            for (const auto& expression : *watches) {
                if (!expression.is_string() || expression.get_ref<const std::string&>().size() > 256) {
                    Write(Error(request, "invalid-script-watches", "Script watches must be strings up to 256 characters"));
                    return;
                }
                const std::string value = expression.get<std::string>();
                if (const auto result = m_scriptHost->EvaluateDebugWatch(value)) {
                    response["scriptWatches"].push_back({ { "expression", result->name }, { "value", result->value }, { "available", true } });
                }
                else {
                    response["scriptWatches"].push_back({ { "expression", value }, { "value", "<unavailable>" }, { "available", false } });
                }
            }
            Write(response);
        }
        else if (*type == "stop") {
            (void)m_uiPreview.Actions().UnregisterProvider("script");
            m_scriptHost.reset();
            m_previewSession.reset();
            m_session.reset();
            m_runtime.reset();
            m_projectRoot.clear();
            m_appliedRevisions.clear();
            m_appliedUiRevisions.clear();
            m_appliedUiDocuments.clear();
            m_appliedPerformanceRevisions.clear();
            m_appliedPerformanceDocuments.clear();
            m_runtimeSources.clear();
            m_showUiPreview = false;
            m_activeUiSceneId.clear();
            m_activeUiRevision = 0;
            m_activePerformanceSceneId.clear();
            m_lastRuntimeState.clear();
            m_lastDebugState.clear();
            m_lastUiDebugState.clear();
            Write(Response(request, "stopAccepted"));
            m_events->Push("state", Json{ { "scope", "host" }, { "state", "stopped" }, { "reason", "stopAccepted" } });
            m_events->Push("debug", Json{ { "scope", "debugger" }, { "state", "detached" }, { "paused", false }, { "reason", "runtimeStopped" } });
        }
        else {
            Write(Error(request, "unknown-request", "Unknown preview request type"));
        }
    }

    void Write(const Json& response) {
        m_stdout.Write(response);
        if (response.contains("diagnostics") && response["diagnostics"].is_array() && !response["diagnostics"].empty()) {
            m_events->Push("diagnostics", Json{ { "sourceResponseType", response.value("type", std::string{}) }, { "diagnostics", response["diagnostics"] } });
            return;
        }
        if (response.value("type", std::string{}) != "error") {
            return;
        }
        m_events->Push(
            "diagnostics",
            Json{ { "sourceResponseType", "error" },
                  { "diagnostics", Json::array({ Json{ { "severity", "error" }, { "code", response.value("code", std::string{ "preview-error" }) }, { "category", "Preview.Protocol" }, { "message", response.value("message", std::string{}) } } }) } }
        );
    }

    void DrainEvents() {
        for (const auto& event : m_events->Drain()) m_stdout.Write(event);
    }

    void QueueExited(const std::string_view reason, const bool expected) {
        if (m_exitEventQueued) return;
        m_exitEventQueued = true;
        m_events->Push("exited", Json{ { "scope", "host" }, { "exitCode", 0 }, { "expected", expected }, { "reason", reason } });
    }

    [[nodiscard]] Json RuntimeStatePayload() const {
        Json state;
        if (!m_session) {
            state["scope"] = "runtime";
            state["state"] = "stopped";
            state["programCounter"] = 0;
            state["sourceLine"] = 0;
            state["sourceId"] = "";
            state["sourceDocumentId"] = "";
            state["script"] = "";
            state["choiceCount"] = 0;
            return state;
        }
        state["scope"] = "runtime";
        state["uiRoute"] = std::string(m_hud.CurrentRoute());
        // Bounded diagnostic snapshot, not a serialized UI document or restore contract.
        state["uiControls"] = Json::array();
        const auto boundedText = [](const std::string& text) {
            auto length = std::min<std::size_t>(text.size(), 512);
            while (length < text.size() && length > 0 &&
                   (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
            return text.substr(0, length);
        };
        std::vector<const px::scene::Node*> pending;
        bool controlsTruncated = false;
        if (m_hud.Root()) pending.push_back(m_hud.Root());
        while (!pending.empty() && state["uiControls"].size() < 64) {
            const auto* node = pending.back();
            pending.pop_back();
            if (const auto* control = dynamic_cast<const px::ui::Control*>(node)) {
                Json item{{"nodeId", control->Id().ToString()}, {"name", control->Name()},
                    {"type", std::string(control->TypeName())}, {"enabled", control->Enabled()},
                    {"visible", control->IsVisibleInTree()}};
                if (const auto* button = dynamic_cast<const px::ui::Button*>(control)) item["text"] = boundedText(button->Text());
                else if (const auto* label = dynamic_cast<const px::ui::Label*>(control)) item["text"] = boundedText(label->Text());
                state["uiControls"].push_back(std::move(item));
            }
            for (const auto& child : node->Children()) {
                if (pending.size() >= 64) { controlsTruncated = true; break; }
                pending.push_back(child.get());
            }
        }
        state["uiControlsTruncated"] = controlsTruncated || !pending.empty();
        state["state"] = VMStateName(m_session->VM().State());
        state["programCounter"] = m_session->VM().ProgramCounter();
        state["sourceLine"] = m_session->VM().CurrentSourceLine();
        const std::string sourceId = m_session->VM().CurrentSourceId();
        const auto source = m_runtimeSources.find(sourceId);
        state["sourceId"] = sourceId;
        state["sourceDocumentId"] = source == m_runtimeSources.end() ? "" : source->second.documentId;
        state["script"] = source == m_runtimeSources.end() || source->second.uri.empty()
            ? m_session->VM().CurrentScript()
            : source->second.uri;
        state["choiceCount"] = m_session->VM().Choices().size();
        return state;
    }

    [[nodiscard]] Json DebugPayload() const {
        Json debug{ { "scope", "debugger" }, { "state", "detached" }, { "paused", false } };
        if (!m_session) return debug;
        debug["state"] = "attached";
        debug["paused"] = m_session->VM().State() == px::vn::VMState::Paused;
        debug["pauseReason"] = m_session->VM().State() == px::vn::VMState::Paused ? (m_session->VM().ManuallyPaused() ? "manual" : "breakpoint") : "";
        debug["programCounter"] = m_session->VM().ProgramCounter();
        debug["sourceLine"] = m_session->VM().CurrentSourceLine();
        const std::string sourceId = m_session->VM().CurrentSourceId();
        const auto source = m_runtimeSources.find(sourceId);
        debug["sourceId"] = sourceId;
        debug["sourceDocumentId"] = source == m_runtimeSources.end() ? "" : source->second.documentId;
        debug["script"] = source == m_runtimeSources.end() || source->second.uri.empty()
            ? m_session->VM().CurrentScript()
            : source->second.uri;
        if (m_scriptHost) {
            const auto& script = m_scriptHost->CaptureDebugState();
            debug["scriptBackend"] = m_scriptHost->BackendId();
            debug["scriptPaused"] = script.paused;
            debug["scriptPauseReason"] = script.reason;
            debug["scriptCallStack"] = Json::array();
            for (const auto& frame : script.frames) {
                Json locals = Json::array();
                for (const auto& local : frame.locals) {
                    locals.push_back({ { "name", local.name }, { "value", local.value } });
                }
                debug["scriptCallStack"].push_back({ { "source", frame.source }, { "function", frame.function }, { "line", frame.line }, { "locals", std::move(locals) } });
            }
        }
        return debug;
    }

    [[nodiscard]] Json RuntimeSnapshotPayload(const px::RuntimeSession::GameState& state) const {
        const auto trackPayload = [](const px::audio::AudioEngine::TrackState& track) {
            return Json{ { "path", track.path },
                         { "loop", track.loop },
                         { "playing", track.playing },
                         { "playbackFrame", track.playbackFrame } };
        };

        Json routes = { { "stack", Json::array() },
                        { "modals", Json::array() },
                        { "stackCount", state.routes.stack.size() },
                        { "modalCount", state.routes.modals.size() },
                        { "truncated", state.routes.stack.size() > kRuntimeSnapshotItemLimit || state.routes.modals.size() > kRuntimeSnapshotItemLimit } };
        for (std::size_t index = 0; index < std::min(state.routes.stack.size(), kRuntimeSnapshotItemLimit); ++index) {
            routes["stack"].push_back(state.routes.stack[index]);
        }
        for (std::size_t index = 0; index < std::min(state.routes.modals.size(), kRuntimeSnapshotItemLimit); ++index) {
            routes["modals"].push_back(state.routes.modals[index]);
        }

        Json playbacks = Json::array();
        for (std::size_t index = 0; index < std::min(state.timelines.size(), kRuntimeSnapshotItemLimit); ++index) {
            const auto& playback = state.timelines[index];
            playbacks.push_back({ { "handle", playback.handle },
                                  { "clipId", playback.clip.ToString() },
                                  { "position", playback.position },
                                  { "speed", playback.speed },
                                  { "loopIteration", playback.loopIteration },
                                  { "playing", playback.playing },
                                  { "awaiting", playback.awaiting } });
        }

        const px::ui::BehaviorRuntimeState behavior = m_showUiPreview && !m_activeUiSceneId.empty()
            ? m_uiPreview.CaptureBehaviorState()
            : state.ui.behavior;
        Json fibers = Json::array();
        for (std::size_t index = 0; index < std::min(behavior.fibers.size(), kRuntimeSnapshotItemLimit); ++index) {
            const auto& fiber = behavior.fibers[index];
            fibers.push_back({ { "id", fiber.id },
                               { "entryNodeId", fiber.entry.Empty() ? Json(nullptr) : Json(fiber.entry.ToString()) },
                               { "currentNodeId", fiber.current.Empty() ? Json(nullptr) : Json(fiber.current.ToString()) },
                               { "continuationDepth", fiber.continuation.size() },
                               { "delayRemaining", fiber.delayRemaining },
                               { "actionExecution", fiber.actionExecution },
                               { "animationHandle", fiber.animationHandle } });
        }
        Json actions = Json::array();
        for (std::size_t index = 0; index < std::min(behavior.actions.size(), kRuntimeSnapshotItemLimit); ++index) {
            const auto& action = behavior.actions[index];
            actions.push_back({ { "execution", action.execution },
                                { "action", action.invocation.action },
                                { "providerId", action.providerId },
                                { "providerHandle", action.providerHandle } });
        }

        const std::size_t activeTrackCount = static_cast<std::size_t>(state.audio.music.playing) +
            static_cast<std::size_t>(state.audio.voice.playing) + static_cast<std::size_t>(state.audio.ambience.playing);
        return Json{ { "schemaRevision", 1 },
                     { "stage",
                       { { "background", state.stage.background },
                         { "previousBackground", state.stage.previousBackground },
                         { "backgroundFade", state.stage.backgroundFade },
                         { "camera", { { "x", state.stage.cameraX }, { "y", state.stage.cameraY }, { "zoom", state.stage.cameraZoom } } },
                         { "shakeActive", state.stage.shakeRemaining > 0.0f },
                         { "screenEffectCount", state.stage.screenEffects.size() },
                         { "actorCount", state.stage.actors.size() },
                         { "layerCount", state.stage.layers.size() },
                         { "tweenCount", state.stage.tweens.size() } } },
                     { "audio",
                       { { "activeTrackCount", activeTrackCount },
                         { "pendingMusicLoop", state.audio.pendingMusicLoop },
                         { "mainVolume", state.audio.mainVolume },
                         { "musicVolume", state.audio.musicVolume },
                         { "voiceVolume", state.audio.voiceVolume },
                         { "sfxVolume", state.audio.sfxVolume },
                         { "ambienceVolume", state.audio.ambienceVolume },
                         { "music", trackPayload(state.audio.music) },
                         { "voice", trackPayload(state.audio.voice) },
                         { "ambience", trackPayload(state.audio.ambience) } } },
                     { "timelines",
                       { { "playbackCount", state.timelines.size() },
                         { "truncated", state.timelines.size() > kRuntimeSnapshotItemLimit },
                         { "playbacks", std::move(playbacks) } } },
                     { "routes", std::move(routes) },
                     { "behavior",
                       { { "source", m_showUiPreview && !m_activeUiSceneId.empty() ? "uiPreview" : "runtime" },
                         { "fiberCount", behavior.fibers.size() },
                         { "actionCount", behavior.actions.size() },
                         { "fibersTruncated", behavior.fibers.size() > kRuntimeSnapshotItemLimit },
                         { "actionsTruncated", behavior.actions.size() > kRuntimeSnapshotItemLimit },
                         { "fibers", std::move(fibers) },
                         { "actions", std::move(actions) } } } };
    }

    [[nodiscard]] Json UiRuntimeDebugPayload() const {
        Json activeNodeIds = Json::array();
        std::set<std::string> uniqueNodeIds;
        for (const auto& fiber : m_uiPreview.CaptureBehaviorState().fibers) {
            if (fiber.current.Empty()) continue;
            if (const std::string nodeId = fiber.current.ToString(); uniqueNodeIds.insert(nodeId).second) {
                activeNodeIds.push_back(nodeId);
            }
        }
        const auto animation = m_uiPreview.CaptureAnimationState();
        return Json{ { "scope", "uiRuntime" },
                     { "sceneId", m_activeUiSceneId },
                     { "appliedRevision", m_activeUiRevision },
                     { "behavior", { { "activeNodeIds", std::move(activeNodeIds) } } },
                     { "animation", { { "activeStateId", animation.state.Empty() ? Json(nullptr) : Json(animation.state.ToString()) }, { "activeTransitionId", animation.transition.Empty() ? Json(nullptr) : Json(animation.transition.ToString()) } } } };
    }

    void EmitRuntimeEventsIfChanged(const std::string_view reason) {
        if (m_session) {
            Json state = RuntimeStatePayload();
            const std::string stateSignature = state.dump();
            if (stateSignature != m_lastRuntimeState) {
                m_lastRuntimeState = stateSignature;
                state["reason"] = reason;
                m_events->Push("state", std::move(state));
            }
            Json debug = DebugPayload();
            const std::string debugSignature = debug.dump();
            if (debugSignature != m_lastDebugState) {
                m_lastDebugState = debugSignature;
                debug["reason"] = reason;
                m_events->Push("debug", std::move(debug));
            }
        }
        if (m_showUiPreview && !m_activeUiSceneId.empty()) {
            Json debug = UiRuntimeDebugPayload();
            const std::string debugSignature = debug.dump();
            if (debugSignature != m_lastUiDebugState) {
                m_lastUiDebugState = debugSignature;
                debug["reason"] = reason;
                m_events->Push("debug", std::move(debug));
            }
        }
    }

    Json RuntimeStateResponse(const Json& request, const std::string& type) const {
        Json response = Response(request, type);
        response.update(RuntimeStatePayload());
        const Json debug = DebugPayload();
        response["scriptBackend"] = debug.value("scriptBackend", std::string{});
        response["scriptPaused"] = debug.value("scriptPaused", false);
        response["scriptPauseReason"] = debug.value("scriptPauseReason", std::string{});
        response["scriptCallStack"] = debug.value("scriptCallStack", Json::array());
        return response;
    }

    void Apply(const Json& request, const bool incremental) {
        spdlog::info("Preview apply request received");
        const auto projectRoot = RequiredString(request, "projectRoot");
        const auto documentId = RequiredString(request, "documentId");
        const auto irPath = RequiredString(request, "irPath");
        const auto revision = request.find("committedRevision");
        if (!projectRoot || !documentId || !irPath || revision == request.end() || !revision->is_number_unsigned()) {
            Write(Error(request, "invalid-apply-request", "projectRoot, documentId, committedRevision and irPath are required"));
            return;
        }
        const std::uint64_t committedRevision = revision->get<std::uint64_t>();
        if (const auto current = m_appliedRevisions.find(*documentId); current != m_appliedRevisions.end() && committedRevision <= current->second) {
            Json response = Response(request, "staleRevision");
            response["documentId"] = *documentId;
            response["requestedRevision"] = committedRevision;
            response["appliedRevision"] = current->second;
            Write(response);
            return;
        }
        const auto current = m_appliedRevisions.find(*documentId);
        if (incremental && (current == m_appliedRevisions.end() || committedRevision != current->second + 1)) {
            Json response = Response(request, "resyncRequired");
            response["documentId"] = *documentId;
            response["requestedRevision"] = committedRevision;
            response["appliedRevision"] = current == m_appliedRevisions.end() ? 0 : current->second;
            response["reason"] = "revisionGap";
            Write(response);
            return;
        }
        const auto loaded = ReadProjectFile(*projectRoot, *irPath);
        if (!loaded) {
            Write(Error(request, "invalid-ir-path", "Runtime IR must be an existing file inside the project root"));
            return;
        }
        const px::sdk::RuntimeIrParseResult parsed = px::sdk::ParseRuntimeIr(loaded->second);
        spdlog::info("Runtime IR parsed operations={}", parsed.document.operations.size());
        if (!parsed.Valid()) {
            Json response = Response(request, "runtimeIrRejected");
            response["documentId"] = *documentId;
            response["requestedRevision"] = committedRevision;
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : parsed.diagnostics) {
                response["diagnostics"].push_back({ { "code", diagnostic.code }, { "message", diagnostic.message }, { "operationIndex", diagnostic.operationIndex } });
            }
            Write(response);
            return;
        }
        if (parsed.document.documentId != *documentId || parsed.document.committedRevision != committedRevision) {
            Write(Error(request, "ir-identity-mismatch", "Runtime IR identity or committed revision does not match the request"));
            return;
        }
        std::unordered_map<std::string, RuntimeSourceLocation> runtimeSources;
        std::optional<std::string> runtimeSourceMapText;
        std::string runtimeSourceMapPath;
        std::filesystem::path sourceMapPath = px::PathFromUtf8(*irPath);
        sourceMapPath.replace_extension(".pxmap");
        std::error_code sourceMapError;
        const bool sourceMapExists = std::filesystem::is_regular_file(
            px::PathFromUtf8(*projectRoot) / sourceMapPath, sourceMapError);
        if (!sourceMapError && sourceMapExists) {
            const auto sourceMapFile = ReadProjectFile(*projectRoot, px::PathToUtf8(sourceMapPath));
            const auto sourceMap = sourceMapFile
                ? px::sdk::ParseSourceMap(sourceMapFile->second)
                : px::sdk::SourceMapParseResult{};
            if (!sourceMapFile || !sourceMap.Valid() ||
                sourceMap.document.documentId != *documentId ||
                sourceMap.document.mappings.size() !=
                    parsed.document.operations.size()) {
                Write(Error(request, "source-map-rejected", "Runtime source map identity or schema is invalid"));
                return;
            }
            for (const auto& entry : sourceMap.document.mappings) {
                if (!runtimeSources.emplace(
                        entry.sourceId,
                        RuntimeSourceLocation{sourceMap.document.documentId,
                                              entry.sourceUri})
                         .second) {
                    Write(Error(request, "source-map-rejected", "Runtime source map sourceId values must be unique"));
                    return;
                }
            }
            for (const auto& operation : parsed.document.operations) {
                const auto* mapping =
                    sourceMap.document.Find(operation.operationId);
                if (!mapping || mapping->sourceId != operation.sourceId ||
                    mapping->startLine != operation.sourceLine) {
                    Write(Error(request, "source-map-rejected", "Runtime source map does not cover every Runtime IR operation"));
                    return;
                }
            }
            runtimeSourceMapText = sourceMapFile->second;
            runtimeSourceMapPath = px::PathToUtf8(sourceMapPath);
        }
        if (!EnsureRuntime(loaded->first)) {
            Write(Error(request, m_runtimeStartupErrorCode.empty() ? "runtime-start-failed" : m_runtimeStartupErrorCode, m_runtimeStartupErrorMessage.empty() ? "Native preview runtime could not start" : m_runtimeStartupErrorMessage));
            return;
        }
        if (runtimeSourceMapText &&
            !m_session->SetSourceMapText(*runtimeSourceMapText,
                                         runtimeSourceMapPath)) {
            Write(Error(request, "source-map-rejected",
                        "Runtime session rejected the canonical source map"));
            return;
        }
        spdlog::info("Native runtime ready");
        const px::sdk::PreviewApplyRequest previewRequest{
            *documentId, committedRevision, loaded->second, *irPath};
        const px::sdk::PreviewApplyResult previewApply =
            incremental ? m_previewSession->Patch(previewRequest)
                        : m_previewSession->Apply(previewRequest);
        if (!previewApply.accepted) {
            Json response = Error(request, "runtime-program-rejected", "RuntimeSession rejected the compiled Runtime IR program");
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : previewApply.diagnostics) {
                response["diagnostics"].push_back(
                    PreviewDiagnosticJson(diagnostic));
            }
            Write(response);
            (void)m_previewSession->Events();
            return;
        }
        spdlog::info("Runtime IR program started");
        if (!previewApply.inPlace) {
            m_previewSaves.clear();
            m_pendingPreviewSave.reset();
            m_previewActions.clear();
        }
        m_runtimeSources = std::move(runtimeSources);
        m_showUiPreview = false;
        m_activeUiSceneId.clear();
        m_activeUiRevision = 0;
        m_lastUiDebugState.clear();
        m_activePerformanceSceneId.clear();
        m_choices.clear();
        (void)m_hud.ShowHUD(DialogueView(*m_session, m_choices));
        m_appliedRevisions[*documentId] = committedRevision;
        Json response = Response(request, "runtimeIrApplied");
        response["documentId"] = *documentId;
        response["appliedRevision"] = committedRevision;
        response["operationCount"] = parsed.document.operations.size();
        response["applyMode"] = !incremental
            ? "fullRuntimeRestart"
            : previewApply.inPlace ? "inPlaceRuntimeIrPatch"
                                   : "structuralRuntimeRestart";
        response["characterCount"] = m_characterCount;
        response["characterResourcesRevision"] = m_characterResourcesDeclared ? px::sdk::kCharacterResourcesContractRevision : 0;
        response["gameCatalogResourcesRevision"] = m_gameCatalogResourcesDeclared ? px::sdk::kGameCatalogResourcesContractRevision : 0;
        response["gameCatalogVariableCount"] = m_gameCatalogVariableCount;
        response["gameCatalogInputBindingCount"] = m_gameCatalogInputBindingCount;
        response["gameCatalogGalleryItemCount"] = m_gameCatalogGalleryItemCount;
        response["renderMode"] = "nativeWindow";
        response["logicalWidth"] = m_resolution.width;
        response["logicalHeight"] = m_resolution.height;
        Write(response);
        (void)m_previewSession->Events();
    }

    void ApplyUiScene(const Json& request) {
        const auto projectRoot = RequiredString(request, "projectRoot");
        const auto sceneId = RequiredString(request, "sceneId");
        const auto uiPath = RequiredString(request, "uiPath");
        const auto revision = request.find("revision");
        if (!projectRoot || !sceneId || !uiPath || revision == request.end() || !revision->is_number_unsigned()) {
            Write(Error(request, "invalid-ui-apply-request", "projectRoot, sceneId, uiPath and revision are required"));
            return;
        }
        const std::uint64_t requestedRevision = revision->get<std::uint64_t>();
        if (const auto current = m_appliedUiRevisions.find(*sceneId); current != m_appliedUiRevisions.end() && requestedRevision <= current->second) {
            Json response = Response(request, "staleUiRevision");
            response["sceneId"] = *sceneId;
            response["requestedRevision"] = requestedRevision;
            response["appliedRevision"] = current->second;
            Write(response);
            return;
        }
        const auto loaded = ReadProjectFile(*projectRoot, *uiPath);
        const auto project = ReadProjectFile(*projectRoot, "project.pxproject");
        if (!loaded || !project) {
            Write(Error(request, "invalid-ui-path", "UI scene and project manifest must exist inside the project root"));
            return;
        }
        const Json authoredDocument = Json::parse(loaded->second, nullptr, false);
        const px::sdk::UiParseResult parsed = px::sdk::ParseUi(loaded->second);
        if (!parsed.Valid()) {
            Json response = Response(request, "uiRejected");
            response["sceneId"] = *sceneId;
            response["requestedRevision"] = requestedRevision;
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : parsed.diagnostics) {
                std::string nodeId;
                if (authoredDocument.is_object() && authoredDocument.contains("nodes") && authoredDocument["nodes"].is_array() && diagnostic.nodeIndex < authoredDocument["nodes"].size()) {
                    nodeId = authoredDocument["nodes"][diagnostic.nodeIndex].value("id", std::string{});
                }
                response["diagnostics"].push_back(UiDiagnostic("error", diagnostic.code, "SDK.UI.Contract", diagnostic.message, "nodeIndex=" + std::to_string(diagnostic.nodeIndex), *sceneId, *uiPath, nodeId));
            }
            Write(response);
            return;
        }
        if (parsed.document.id != *sceneId || parsed.document.revision != requestedRevision) {
            Write(Error(request, "ui-identity-mismatch", "UI scene identity or revision does not match the request"));
            return;
        }
        Json manifest = Json::parse(project->second, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) {
            Write(Error(request, "invalid-project-manifest", "Project manifest is not valid JSON"));
            return;
        }
        std::unordered_map<std::string, std::string> assets;
        if (manifest.contains("assets") && manifest["assets"].is_array()) {
            for (const auto& asset : manifest["assets"]) {
                if (!asset.contains("id") || !asset["id"].is_string() || !asset.contains("source") || !asset["source"].is_string()) continue;
                const auto source = SafeProjectRelativePath(asset["source"].get<std::string>());
                if (source && IsProjectRegularFile(loaded->first, *source)) assets.emplace(asset["id"].get<std::string>(), *source);
            }
        }
        std::unordered_map<std::string, std::string> components;
        if (manifest.contains("uiComponents") && manifest["uiComponents"].is_array()) {
            for (const auto& component : manifest["uiComponents"]) {
                if (!component.is_object() || !component.contains("id") || !component["id"].is_string() || !component.contains("source") || !component["source"].is_string()) continue;
                const auto source = SafeProjectRelativePath(component["source"].get<std::string>());
                if (source && IsProjectRegularFile(loaded->first, *source)) components.emplace(component["id"].get<std::string>(), *source);
            }
        }
        if (!EnsureRuntime(loaded->first)) {
            Write(Error(request, m_runtimeStartupErrorCode.empty() ? "runtime-start-failed" : m_runtimeStartupErrorCode, m_runtimeStartupErrorMessage.empty() ? "Native preview runtime could not start" : m_runtimeStartupErrorMessage));
            return;
        }
        const std::uint64_t actionCountBefore = m_uiActionCount;
        const auto previewAction = [this](const px::ui::ActionInvocation& invocation) {
            if (invocation.context.signal != "ui.onClick") {
                m_lastUiAction = invocation.action;
                ++m_uiActionCount;
            }
            spdlog::info("Native UI preview behavior action {}", invocation.action);
            return px::Status::Ok();
        };
        if (auto provider = std::dynamic_pointer_cast<px::ui::BuiltInActionProvider>(m_uiPreview.Actions().FindProvider("builtin"))) {
            provider->SetFallback(previewAction);
        }
        else {
            auto createdProvider = std::make_shared<px::ui::BuiltInActionProvider>();
            createdProvider->SetFallback(previewAction);
            if (const auto status = m_uiPreview.Actions().RegisterProvider(std::move(createdProvider)); !status) {
                Write(Error(request, "ui-action-provider-rejected", StatusMessage(status, "Runtime UI action provider could not be configured")));
                return;
            }
        }
        const auto applicationOptions = [&]() {
            return px::ui::UiApplicationOptions{ .sourcePath = *uiPath,
                                                       .resolveAsset = [&assets](const std::string_view assetId) -> std::optional<std::string> {
                                                           const auto found = assets.find(std::string(assetId));
                                                           return found == assets.end() ? std::nullopt : std::optional<std::string>{ found->second };
                                                       },
                                                       .loadComponent = [&components, projectRoot = *projectRoot](const std::string_view componentId) -> std::optional<px::ui::UiComponentSource> {
                                                           const auto found = components.find(std::string(componentId));
                                                           if (found == components.end()) return std::nullopt;
                                                           const auto source = ReadProjectFile(projectRoot, found->second);
                                                           return source ? std::optional<px::ui::UiComponentSource>{ px::ui::UiComponentSource{ found->second, source->second } } : std::nullopt;
                                                       },
                                                       .loadMotion = [projectRoot = *projectRoot](const std::string_view path) -> std::optional<std::string> {
                                                           const auto source = ReadProjectFile(projectRoot, std::string(path));
                                                           return source ? std::optional<std::string>{source->second} : std::nullopt;
                                                       },
                                                       .viewModel = &m_uiPreviewViewModel,
                                                       .previewSafeMode = true,
                                                       .diagnosticOverlay = false,
                                                       .observeAction =
                                                           [this](const px::sdk::UiAction& action, const px::Status& status) {
                                                               if (!status) {
                                                                   spdlog::warn("Native UI preview Action {} was rejected: {}", action.id, StatusMessage(status, "unknown error"));
                                                                   return;
                                                               }
                                                               m_lastUiAction = action.id;
                                                               ++m_uiActionCount;
                                                               spdlog::info("Native UI preview Action {} dispatched", action.id);
                                                           } };
        };
        UiPreviewUpdatePlan updatePlan;
        if (m_activeUiSceneId == *sceneId) {
            if (const auto previous = m_appliedUiDocuments.find(*sceneId); previous != m_appliedUiDocuments.end())
                updatePlan = PlanUiPreviewUpdate(previous->second, authoredDocument);
            else
                updatePlan.reason = "missingPreviousDocument";
        }
        else if (!m_activeUiSceneId.empty()) {
            updatePlan.reason = "activeSceneChanged";
        }
        px::ui::UiApplication application(m_uiPreview);
        auto applied = updatePlan.patch ? application.PatchDocumentProperties(parsed.document, applicationOptions()) : application.ApplyDocument(parsed.document, applicationOptions());
        bool patchFallback = false;
        if (!applied && updatePlan.patch) {
            for (const auto& diagnostic : applied.Diagnostics()) spdlog::warn("Native UI preview property patch rejected: {} {}", diagnostic.code, diagnostic.message);
            patchFallback = true;
            updatePlan.patch = false;
            updatePlan.reason = "propertyPatchFailed";
            applied = application.ApplyDocument(parsed.document, applicationOptions());
        }
        if (!applied) {
            Json response = Response(request, "uiRejected");
            response["sceneId"] = *sceneId;
            response["requestedRevision"] = requestedRevision;
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : applied.Diagnostics()) {
                response["diagnostics"].push_back(UiDiagnostic(
                    px::diag::ToString(diagnostic.severity),
                    diagnostic.code,
                    diagnostic.category,
                    diagnostic.message,
                    diagnostic.details,
                    diagnostic.source.resourceId.empty() ? std::string_view(*sceneId) : std::string_view(diagnostic.source.resourceId),
                    diagnostic.source.path.empty() ? std::string_view(*uiPath) : std::string_view(diagnostic.source.path),
                    diagnostic.source.nodeId,
                    diagnostic.source.property,
                    diagnostic.source.line,
                    diagnostic.source.column
                ));
            }
            Write(response);
            return;
        }
        const auto summary = applied.TakeValue();
        m_showUiPreview = true;
        m_activeUiSceneId = *sceneId;
        m_activeUiRevision = requestedRevision;
        m_activePerformanceSceneId.clear();
        m_lastUiDebugState.clear();
        m_appliedUiRevisions[*sceneId] = requestedRevision;
        m_appliedUiDocuments[*sceneId] = authoredDocument;
        Json response = Response(request, "uiApplied");
        response["sceneId"] = *sceneId;
        response["appliedRevision"] = requestedRevision;
        response["nodeCount"] = summary.nodeCount;
        response["actionBindingCount"] = summary.actionBindingCount;
        response["behaviorNodeCount"] = summary.behaviorNodeCount;
        response["behaviorTriggerCount"] = summary.behaviorTriggerCount;
        response["animationClipCount"] = summary.animationClipCount;
        response["animationTrackCount"] = summary.animationTrackCount;
        response["actionsDispatchedDuringApply"] = m_uiActionCount - actionCountBefore;
        response["renderMode"] = "nativeWindow";
        response["updateKind"] = updatePlan.patch ? "patch" : "reload";
        response["changedNodeCount"] = updatePlan.changedNodeCount;
        response["reloadReason"] = updatePlan.patch ? "" : updatePlan.reason;
        response["patchFallback"] = patchFallback;
        Write(response);
        m_events->Push("state", Json{ { "scope", "uiPreview" }, { "state", updatePlan.patch ? "patched" : "reloaded" }, { "sceneId", *sceneId }, { "appliedRevision", requestedRevision } });
    }

    void ActivateUiControl(const Json& request) {
        const auto nodeId = RequiredString(request, "nodeId");
        if (!nodeId) {
            Write(Error(request, "invalid-ui-control-activation", "nodeId is required"));
            return;
        }
        if (!m_showUiPreview && m_previewSession && m_hud.Root()) {
            const auto state = m_previewSession->State();
            if (request.value("documentId", std::string{}) != state.documentId ||
                request.value("revision", 0ULL) != state.revision) {
                Write(Error(request, "ui-preview-identity-mismatch", "Control activation must target the applied Runtime revision"));
                return;
            }
            const auto activated = m_hud.ActivateUiControl(*nodeId);
            if (!activated) {
                Write(Error(request, "ui-control-not-activatable", StatusMessage(activated, "Control action was rejected")));
                return;
            }
            Write(RuntimeStateResponse(request, "uiControlActivated"));
            return;
        }
        if (!m_showUiPreview || m_activeUiSceneId.empty() || !m_uiPreview.Root()) {
            Write(Error(request, "ui-preview-not-active", "Apply a UI scene before activating a control"));
            return;
        }
        if (request.value("documentId", std::string{}) != m_activeUiSceneId || request.value("revision", 0ULL) != m_activeUiRevision) {
            Write(Error(request, "ui-preview-identity-mismatch", "Control activation must target the active UI scene revision"));
            return;
        }
        const auto parsedNodeId = px::Uuid::Parse(*nodeId);
        if (!parsedNodeId) {
            Write(Error(request, "invalid-ui-control-id", "nodeId must be a canonical Runtime UUID"));
            return;
        }
        auto* button = dynamic_cast<px::ui::Button*>(m_uiPreview.Root()->Find(*parsedNodeId));
        if (!button) {
            Write(Error(request, "ui-control-not-activatable", "nodeId must identify a Button-compatible control in the active UI scene"));
            return;
        }
        const std::uint64_t actionCountBefore = m_uiActionCount;
        button->Activate();
        if (m_uiActionCount == actionCountBefore) {
            Write(Error(request, "ui-control-action-not-dispatched", "The selected control is disabled, unbound, or its Action was rejected"));
            return;
        }
        Json response = Response(request, "uiControlActivated");
        response["sceneId"] = m_activeUiSceneId;
        response["nodeId"] = *nodeId;
        response["actionId"] = m_lastUiAction;
        Write(response);
        m_events->Push("state", Json{ { "scope", "uiPreview" },
                                      { "state", "controlActivated" },
                                      { "sceneId", m_activeUiSceneId },
                                      { "nodeId", *nodeId },
                                      { "actionId", m_lastUiAction },
                                      { "appliedRevision", m_activeUiRevision } });
    }

    void SeekPerformance(const Json& request) {
        const auto projectRoot = RequiredString(request, "projectRoot");
        const auto sceneId = RequiredString(request, "sceneId");
        const auto performancePath = RequiredString(request, "performancePath");
        const auto revision = request.find("revision");
        const auto time = request.find("time");
        if (!projectRoot || !sceneId || !performancePath || revision == request.end() || !revision->is_number_unsigned() || time == request.end() || !time->is_number()) {
            Write(Error(request, "invalid-performance-seek", "projectRoot, sceneId, performancePath, revision and time are required"));
            return;
        }
        const auto loaded = ReadProjectFile(*projectRoot, *performancePath);
        const auto project = ReadProjectFile(*projectRoot, "project.pxproject");
        if (!loaded || !project) {
            Write(Error(request, "invalid-performance-path", "Performance and project manifest must exist inside the project root"));
            return;
        }
        Json document = Json::parse(loaded->second, nullptr, false);
        const std::uint64_t requestedRevision = revision->get<std::uint64_t>();
        const double requestedTime = time->get<double>();
        if (const auto current = m_appliedPerformanceRevisions.find(*sceneId);
            current != m_appliedPerformanceRevisions.end() && requestedRevision < current->second) {
            Write(Error(request, "stale-performance-revision", "Performance revision is older than the applied Runtime state"));
            return;
        }
        if (!EnsureRuntime(loaded->first)) {
            Write(Error(request, m_runtimeStartupErrorCode.empty() ? "runtime-start-failed" : m_runtimeStartupErrorCode, m_runtimeStartupErrorMessage.empty() ? "Native preview runtime could not start" : m_runtimeStartupErrorMessage));
            return;
        }
        const auto previewPlan = px::preview::BuildPerformancePreviewPlan(
            loaded->second, project->second, m_gameCatalog, *sceneId,
            requestedTime, [this](const std::string_view path) {
                return m_runtime && m_runtime->VFS().Exists(std::string(path));
            });
        if (!previewPlan) {
            const auto& diagnostics = previewPlan.Diagnostics();
            Write(Error(
                request,
                diagnostics.empty() ? "invalid-performance-preview"
                                    : diagnostics.front().code,
                diagnostics.empty()
                    ? "Performance could not be planned by RuntimeCore"
                    : diagnostics.front().message));
            return;
        }
        if (previewPlan.Value().revision != requestedRevision) {
            Write(Error(request, "performance-identity-mismatch",
                        "Performance revision does not match the requested revision"));
            return;
        }
        const auto& plan = previewPlan.Value();
        const double seekTime = plan.seekTime;

        std::uint64_t performanceUiRevision = 0;
        if (!plan.uiSceneId.empty()) {
            const auto uiPath = RequiredString(request, "uiPath");
            if (!uiPath) {
                Write(Error(request, "missing-performance-ui-scene",
                            "A Performance with stage.uiSceneId requires a project-relative uiPath"));
                return;
            }
            const auto loadedUi = ReadProjectFile(*projectRoot, *uiPath);
            if (!loadedUi) {
                Write(Error(request, "invalid-performance-ui-path",
                            "The bound Performance UI scene must exist inside the project root"));
                return;
            }
            const auto parsedUi = px::sdk::ParseUi(loadedUi->second);
            if (!parsedUi.Valid() || parsedUi.document.id != plan.uiSceneId) {
                Write(Error(request, "performance-ui-identity-mismatch",
                            "The UI scene identity does not match stage.uiSceneId"));
                return;
            }
            performanceUiRevision = parsedUi.document.revision;
            const Json manifest = Json::parse(project->second, nullptr, false);
            std::unordered_map<std::string, std::string> assets;
            if (manifest.is_object() && manifest.contains("assets") &&
                manifest["assets"].is_array()) {
                for (const auto& asset : manifest["assets"]) {
                    if (!asset.is_object() || !asset.contains("id") ||
                        !asset["id"].is_string() ||
                        !asset.contains("source") ||
                        !asset["source"].is_string())
                        continue;
                    const auto source = SafeProjectRelativePath(
                        asset["source"].get<std::string>());
                    if (source && IsProjectRegularFile(loadedUi->first, *source))
                        assets.emplace(asset["id"].get<std::string>(),
                                       *source);
                }
            }
            std::unordered_map<std::string, std::string> components;
            if (manifest.is_object() && manifest.contains("uiComponents") &&
                manifest["uiComponents"].is_array()) {
                for (const auto& component : manifest["uiComponents"]) {
                    if (!component.is_object() || !component.contains("id") ||
                        !component["id"].is_string() ||
                        !component.contains("source") ||
                        !component["source"].is_string())
                        continue;
                    const auto source = SafeProjectRelativePath(
                        component["source"].get<std::string>());
                    if (source && IsProjectRegularFile(loadedUi->first, *source))
                        components.emplace(
                            component["id"].get<std::string>(), *source);
                }
            }
            m_hud.SetUiAssetResolver(
                [assets = std::move(assets)](
                    const std::string_view id) -> std::optional<std::string> {
                    const auto found = assets.find(std::string(id));
                    return found == assets.end()
                               ? std::nullopt
                               : std::optional<std::string>{found->second};
                });
            m_hud.SetUiComponentLoader(
                [components = std::move(components),
                 root = *projectRoot](const std::string_view id)
                    -> std::optional<px::ui::UiComponentSource> {
                    const auto found = components.find(std::string(id));
                    if (found == components.end()) return std::nullopt;
                    const auto source = ReadProjectFile(root, found->second);
                    return source
                               ? std::optional<px::ui::UiComponentSource>{
                                     px::ui::UiComponentSource{
                                         found->second, source->second}}
                               : std::nullopt;
                });
            m_hud.SetUiMotionLoader(
                [root = *projectRoot](const std::string_view path) -> std::optional<std::string> {
                    const auto source = ReadProjectFile(root, std::string(path));
                    return source ? std::optional<std::string>{source->second} : std::nullopt;
                });
            const auto registered = m_hud.RegisterTemplate(
                px::ui::GalgameUI::Screen::Title, loadedUi->second, *uiPath);
            const auto shown = registered ? m_hud.ShowTitle() : registered;
            if (!shown) {
                Write(Error(request, "performance-ui-apply-failed",
                            StatusMessage(shown,
                                          "The bound UI scene could not be installed")));
                return;
            }
        } else {
            (void)m_hud.ShowHUD(DialogueView(*m_session, m_choices));
        }

        m_showUiPreview = false;
        m_activeUiSceneId.clear();
        m_activeUiRevision = 0;
        m_lastUiDebugState.clear();
        UiPreviewUpdatePlan updatePlan;
        if (m_activePerformanceSceneId == *sceneId) {
            if (const auto previous = m_appliedPerformanceDocuments.find(*sceneId); previous != m_appliedPerformanceDocuments.end())
                updatePlan = PlanPerformancePreviewUpdate(previous->second, document);
            else
                updatePlan.reason = "missingPreviousPerformance";
        }
        else if (!m_activePerformanceSceneId.empty() || m_session) {
            updatePlan.reason = "activePreviewChanged";
        }
        px::preview::ApplyPerformancePreviewPlan(*m_session, plan);
        if (!plan.uiSceneId.empty()) {
            const auto uiStatus =
                px::preview::ApplyPerformancePreviewUiPlan(m_hud, plan);
            if (!uiStatus) {
                Write(Error(request, "performance-ui-plan-rejected",
                            StatusMessage(uiStatus,
                                          "The sampled UI Timeline plan was rejected")));
                return;
            }
        }
        const std::size_t nodeCount = plan.nodes.size();
        Json response = Response(request, "performanceSeeked");
        response["sceneId"] = *sceneId;
        response["revision"] = requestedRevision;
        response["time"] = seekTime;
        response["nodeCount"] = nodeCount;
        response["unsafeEventsSkipped"] = plan.unsafeEventsSkipped;
        response["uiSceneId"] = plan.uiSceneId;
        response["uiRevision"] = performanceUiRevision;
        response["uiControls"] = Json::array();
        for (const auto& control : plan.uiControls) {
            response["uiControls"].push_back(
                {{"clipId", control.clipId},
                 {"targetId", control.targetId},
                 {"visible", control.visible}});
        }
        response["uiAnimation"] = plan.uiAnimation
                                      ? Json{{"clipId", plan.uiAnimation->clipId},
                                             {"animationClipId", plan.uiAnimation->animationClipId},
                                             {"offsetSeconds", plan.uiAnimation->offsetSeconds},
                                             {"playing", plan.uiAnimation->playing}}
                                      : Json(nullptr);
        response["updateKind"] = updatePlan.patch ? "patch" : "reload";
        response["changedNodeCount"] = updatePlan.changedNodeCount;
        response["reloadReason"] = updatePlan.patch ? "" : updatePlan.reason;
        m_activePerformanceSceneId = *sceneId;
        m_appliedPerformanceRevisions[*sceneId] = requestedRevision;
        m_appliedPerformanceDocuments[*sceneId] = document;
        Write(response);
        m_events->Push("state", Json{ { "scope", "performancePreview" },
                                      { "state", updatePlan.patch ? "patched" : "reloaded" },
                                      { "sceneId", *sceneId },
                                      { "revision", requestedRevision },
                                      { "time", seekTime } });
    }

    bool LoadCharacterCatalog(px::Runtime& runtime, px::vn::GameCatalog& catalog, bool& characterResourcesDeclared) {
        const auto projectManifest = runtime.VFS().ReadText("project.pxproject");
        if (!projectManifest) {
            m_runtimeStartupErrorCode = "PXCHAR1032";
            m_runtimeStartupErrorMessage = "Required project manifest is missing: project.pxproject";
            return false;
        }
        const auto characterResources = catalog.LoadCharacterResources(
            *projectManifest, [&runtime](const std::string_view uri) { return runtime.VFS().ReadText(std::string(uri)); }, [&runtime](const std::string_view uri) { return runtime.VFS().Exists(std::string(uri)); }, characterResourcesDeclared
        );
        if (!characterResources) {
            m_runtimeStartupErrorCode = characterResources.Diagnostics().empty() ? "invalid-character-resources" : characterResources.Diagnostics().front().code;
            m_runtimeStartupErrorMessage = StatusMessage(characterResources, "characterResources validation failed");
            return false;
        }
        const Json project = Json::parse(*projectManifest, nullptr, false);
        const std::string catalogPath =
            project.is_object()
                ? project.value("gameCatalog", std::string{})
                : std::string{};
        const auto runtimeCatalog = catalogPath.empty()
                                        ? std::optional<std::string>{}
                                        : runtime.VFS().ReadText(catalogPath);
        if (!runtimeCatalog || !catalogPath.ends_with(".pxgame")) {
            m_runtimeStartupErrorCode = "PXCAT1120";
            m_runtimeStartupErrorMessage =
                "Canonical project gameCatalog is missing from Preview BuildArtifact";
            return false;
        }
        const auto catalogStatus = catalog.LoadCanonical(
            *runtimeCatalog, *projectManifest,
            [&runtime](const std::string_view uri) {
                return runtime.VFS().Exists(std::string(uri));
            },
            catalogPath);
        if (!catalogStatus) {
            m_runtimeStartupErrorCode =
                catalogStatus.Diagnostics().empty()
                    ? "invalid-game-catalog"
                    : catalogStatus.Diagnostics().front().code;
            m_runtimeStartupErrorMessage =
                StatusMessage(catalogStatus,
                              "Canonical GameCatalog is invalid");
            return false;
        }
        m_gameCatalogResourcesDeclared = true;
        m_gameCatalogVariableCount = catalog.Variables().size();
        m_gameCatalogInputBindingCount = catalog.InputBindings().size();
        m_gameCatalogGalleryItemCount = catalog.Gallery().size();
        return true;
    }

    struct PreviewExtensionSet {
        std::vector<std::string> manifests;
        std::string buildIdentity;
    };

    std::optional<PreviewExtensionSet> ReadPreviewExtensions(px::Runtime& runtime) {
        const auto projectText = runtime.VFS().ReadText("project.pxproject");
        const Json project = projectText
                                 ? Json::parse(*projectText, nullptr, false)
                                 : Json(Json::value_t::discarded);
        if (!projectText || project.is_discarded() || !project.is_object() ||
            project.value("format", std::string{}) != "PrismatiXProject" ||
            project.value("schemaRevision", 0) != 1 ||
            !project.contains("extensions") || !project["extensions"].is_array() ||
            project["extensions"].size() > 1024) {
            m_runtimeStartupErrorCode = "PXPREVIEW-EXT-001";
            m_runtimeStartupErrorMessage =
                "Preview requires the canonical project extension list";
            return std::nullopt;
        }

        PreviewExtensionSet result;
        std::set<std::string> uniqueManifests;
        const auto appendIdentity = [&result](const std::string_view label,
                                              const std::string_view value) {
            result.buildIdentity.append(std::to_string(label.size()))
                .push_back(':');
            result.buildIdentity.append(label).push_back('=');
            result.buildIdentity.append(std::to_string(value.size()))
                .push_back(':');
            result.buildIdentity.append(value).push_back(';');
        };
        for (const auto& encodedPath : project["extensions"]) {
            if (!encodedPath.is_string()) {
                m_runtimeStartupErrorCode = "PXPREVIEW-EXT-002";
                m_runtimeStartupErrorMessage =
                    "Project extension paths must be strings";
                return std::nullopt;
            }
            const std::string manifestPath = encodedPath.get<std::string>();
            const auto normalized = px::io::VFS::NormalizeVirtualPath(manifestPath);
            if (!normalized || *normalized != manifestPath ||
                !manifestPath.ends_with(".pxextension") ||
                !uniqueManifests.insert(manifestPath).second) {
                m_runtimeStartupErrorCode = "PXPREVIEW-EXT-003";
                m_runtimeStartupErrorMessage =
                    "Project extension paths must be unique canonical .pxextension paths";
                return std::nullopt;
            }
            const auto manifestText = runtime.VFS().ReadText(manifestPath);
            const Json manifest = manifestText
                                      ? Json::parse(*manifestText, nullptr, false)
                                      : Json(Json::value_t::discarded);
            if (!manifestText || manifest.is_discarded() || !manifest.is_object() ||
                !manifest.contains("entry") || !manifest["entry"].is_string()) {
                m_runtimeStartupErrorCode = "PXPREVIEW-EXT-004";
                m_runtimeStartupErrorMessage =
                    "A declared Preview extension manifest is missing or corrupt: " +
                    manifestPath;
                return std::nullopt;
            }
            result.manifests.push_back(manifestPath);
            appendIdentity(manifestPath, *manifestText);

            std::vector<std::string> modules{
                manifest["entry"].get<std::string>()};
            if (const auto declared = manifest.find("modules");
                declared != manifest.end()) {
                if (!declared->is_array() || declared->size() > 1024) {
                    m_runtimeStartupErrorCode = "PXPREVIEW-EXT-005";
                    m_runtimeStartupErrorMessage =
                        "Extension modules must be a bounded array: " + manifestPath;
                    return std::nullopt;
                }
                for (const auto& module : *declared) {
                    if (!module.is_string()) {
                        m_runtimeStartupErrorCode = "PXPREVIEW-EXT-006";
                        m_runtimeStartupErrorMessage =
                            "Extension module paths must be strings: " + manifestPath;
                        return std::nullopt;
                    }
                    modules.push_back(module.get<std::string>());
                }
            }
            const auto separator = manifestPath.find_last_of('/');
            std::set<std::string> uniqueModules;
            for (const auto& module : modules) {
                const auto relative = px::io::VFS::NormalizeVirtualPath(module);
                const std::string resolved =
                    separator == std::string::npos
                        ? module
                        : manifestPath.substr(0, separator + 1) + module;
                const auto normalizedModule =
                    px::io::VFS::NormalizeVirtualPath(resolved);
                if (!relative || *relative != module || !module.ends_with(".js") ||
                    !normalizedModule ||
                    (!uniqueModules.insert(*normalizedModule).second &&
                     *normalizedModule !=
                         (separator == std::string::npos
                              ? manifest["entry"].get<std::string>()
                              : manifestPath.substr(0, separator + 1) +
                                    manifest["entry"].get<std::string>()))) {
                    m_runtimeStartupErrorCode = "PXPREVIEW-EXT-007";
                    m_runtimeStartupErrorMessage =
                        "Extension module path is unsafe or duplicated: " + module;
                    return std::nullopt;
                }
                const auto source = runtime.VFS().ReadText(*normalizedModule);
                if (!source) {
                    m_runtimeStartupErrorCode = "PXPREVIEW-EXT-008";
                    m_runtimeStartupErrorMessage =
                        "Extension module is missing: " + *normalizedModule;
                    return std::nullopt;
                }
                appendIdentity(*normalizedModule, *source);
            }
        }
        return result;
    }

    struct PreviewResolution {
        int width = 1280;
        int height = 720;
        bool operator==(const PreviewResolution&) const = default;
    };

    // The compiled project's logical resolution, which the Player uses as its
    // canvas. Stage sprites and authored UI are laid out in these coordinates,
    // so Preview must render the same canvas (letterboxed into its window).
    std::optional<PreviewResolution> ReadProjectResolution(
        const std::filesystem::path& projectRoot) {
        const auto projectFile =
            ReadProjectFile(px::PathToUtf8(projectRoot), "project.pxproject");
        const Json project = projectFile
                                 ? Json::parse(projectFile->second, nullptr, false)
                                 : Json(Json::value_t::discarded);
        PreviewResolution resolution;
        if (!project.is_object()) return resolution;
        const auto declared = project.find("resolution");
        if (declared == project.end()) return resolution;
        const auto dimension = [&declared](const char* name) {
            const auto value = declared->find(name);
            return value != declared->end() && value->is_number_integer()
                       ? value->get<std::int64_t>()
                       : std::int64_t{0};
        };
        const auto width = declared->is_object() ? dimension("width") : 0;
        const auto height = declared->is_object() ? dimension("height") : 0;
        if (width < 16 || height < 16 || width > 16384 || height > 16384) {
            m_runtimeStartupErrorCode = "PXPREVIEW-RESOLUTION-001";
            m_runtimeStartupErrorMessage =
                "Compiled project resolution must be integer width and height between 16 and 16384";
            return std::nullopt;
        }
        resolution.width = static_cast<int>(width);
        resolution.height = static_cast<int>(height);
        return resolution;
    }

    // The Preview window keeps the project's aspect ratio. A first window fits
    // within 1280x720 so a 1080p or 4K project does not open a window larger
    // than the editor next to it; a runtime rebuilt for a new revision fits
    // within the window the author already had. The canvas itself always
    // stays at project resolution and is letterboxed into the window.
    static std::pair<int, int> PreviewWindowSize(
        const PreviewResolution& resolution,
        const std::optional<std::pair<int, int>>& previousWindow = std::nullopt) {
        double scale = std::min({1.0, 1280.0 / resolution.width,
                                 720.0 / resolution.height});
        if (previousWindow && previousWindow->first > 0 && previousWindow->second > 0)
            scale = std::min(static_cast<double>(previousWindow->first) / resolution.width,
                             static_cast<double>(previousWindow->second) / resolution.height);
        return {std::max(1, static_cast<int>(std::lround(resolution.width * scale))),
                std::max(1, static_cast<int>(std::lround(resolution.height * scale)))};
    }

    std::optional<std::pair<int, int>> CurrentWindowSize() const {
        if (!m_runtime) return std::nullopt;
        int width = 0;
        int height = 0;
        m_runtime->GetWindow().GetSize(width, height);
        if (width <= 0 || height <= 0) return std::nullopt;
        return std::pair{width, height};
    }

    void ApplyResolution(const PreviewResolution& resolution) {
        m_resolution = resolution;
        m_hud.SetBuiltinCanvas(resolution.width, resolution.height);
        if (!m_runtime) return;
        const auto previousWindow = CurrentWindowSize();
        m_runtime->Renderer().SetLogicalSize(resolution.width, resolution.height);
        m_runtime->GetWindow().SetAspectRatio(static_cast<float>(resolution.width) /
                                              static_cast<float>(resolution.height));
        const auto [width, height] = PreviewWindowSize(resolution, previousWindow);
        (void)m_runtime->GetWindow().Resize(width, height);
    }

    bool EnsureRuntime(const std::filesystem::path& projectRoot) {
        m_runtimeStartupErrorCode.clear();
        m_runtimeStartupErrorMessage.clear();
        const auto resolution = ReadProjectResolution(projectRoot);
        if (!resolution) return false;
        if (m_runtime && m_projectRoot == projectRoot) {
            const auto extensions = ReadPreviewExtensions(*m_runtime);
            if (!extensions) return false;
            // A re-applied revision may change the project resolution.
            if (*resolution != m_resolution) ApplyResolution(*resolution);
            if (extensions->buildIdentity != m_extensionBuildIdentity) {
                spdlog::info(
                    "Preview extension graph changed; rebuilding candidate Runtime session");
            } else {
            px::vn::GameCatalog catalog;
            bool characterResourcesDeclared = false;
            if (!LoadCharacterCatalog(*m_runtime, catalog, characterResourcesDeclared)) {
                return false;
            }
            m_gameCatalog = std::move(catalog);
            m_session->VM().SetGameCatalog(m_gameCatalog);
            m_characterCount = m_gameCatalog.Characters().size();
            m_characterResourcesDeclared = characterResourcesDeclared;
            return true;
            }
        }
        m_runtimeStartupErrorCode.clear();
        m_runtimeStartupErrorMessage.clear();
        (void)m_uiPreview.Actions().UnregisterProvider("script");
        m_scriptHost.reset();
        m_previewSession.reset();
        m_previewSaves.clear();
        m_pendingPreviewSave.reset();
        m_previewActions.clear();
        m_session.reset();
        const auto previousWindow = CurrentWindowSize();
        m_runtime.reset();
        m_appliedRevisions.clear();
        m_appliedUiRevisions.clear();
        m_appliedUiDocuments.clear();
        m_appliedPerformanceRevisions.clear();
        m_appliedPerformanceDocuments.clear();
        m_runtimeSources.clear();
        m_showUiPreview = false;
        m_activeUiSceneId.clear();
        m_activeUiRevision = 0;
        m_activePerformanceSceneId.clear();
        m_lastRuntimeState.clear();
        m_lastDebugState.clear();
        m_lastUiDebugState.clear();
        m_characterCount = 0;
        m_characterResourcesDeclared = false;
        m_gameCatalogResourcesDeclared = false;
        m_gameCatalogVariableCount = 0;
        m_gameCatalogInputBindingCount = 0;
        m_gameCatalogGalleryItemCount = 0;
        m_extensionBuildIdentity.clear();
        auto runtime = std::make_unique<px::Runtime>();
        px::RuntimeConfig config;
        config.title = "PrismatiX Preview";
        config.mountDirs = { px::PathToUtf8(projectRoot) };
        config.logicalWidth = resolution->width;
        config.logicalHeight = resolution->height;
        std::tie(config.width, config.height) =
            PreviewWindowSize(*resolution, previousWindow);
        spdlog::info("Initializing native runtime projectRoot={}", px::PathToUtf8(projectRoot));
        if (!runtime->Init(config)) {
            m_runtimeStartupErrorMessage = "Native preview runtime could not initialize";
            return false;
        }
        // Like the Player: built-in screens lay out on the project canvas.
        m_resolution = *resolution;
        m_hud.SetBuiltinCanvas(resolution->width, resolution->height);
        px::vn::GameCatalog catalog;
        bool characterResourcesDeclared = false;
        if (!LoadCharacterCatalog(*runtime, catalog, characterResourcesDeclared)) {
            return false;
        }
        const auto previewExtensions = ReadPreviewExtensions(*runtime);
        if (!previewExtensions) return false;
        spdlog::info("Constructing runtime session");
        auto session = std::make_unique<px::RuntimeSession>(px::RuntimeSession::Services{ runtime->VFS(), runtime->Audio(), runtime->Renderer(), runtime->Assets() });
        m_gameCatalog = std::move(catalog);
        session->VM().SetGameCatalog(m_gameCatalog);
        // The compiled project carries the same locale and font chain used by
        // Player. A Preview launched from a clean npm install must not depend
        // on fonts or locale files from an engine checkout.
        const auto projectText = runtime->VFS().ReadText("project.pxproject");
        const auto project = projectText ? Json::parse(*projectText, nullptr, false) : Json{};
        if (!project.is_object() || !project.contains("defaultLocale") ||
            !project["defaultLocale"].is_string()) {
            m_runtimeStartupErrorMessage = "Compiled project has no default locale";
            return false;
        }
        const auto locale = project["defaultLocale"].get<std::string>();
        const auto localeFile = ReadProjectFile(px::PathToUtf8(projectRoot), "Content/Localization/" + locale + ".json");
        const auto localeDocument = localeFile ? Json::parse(localeFile->second, nullptr, false) : Json{};
        if (!localeDocument.is_object() ||
            localeDocument.value("format", std::string{}) != "PrismatiXLocale" ||
            localeDocument.value("schemaRevision", 0) != 1 ||
            localeDocument.value("locale", std::string{}) != locale ||
            !localeDocument.contains("strings") || !localeDocument["strings"].is_object()) {
            m_runtimeStartupErrorMessage = "Compiled project locale is missing or invalid";
            return false;
        }
        std::unordered_map<std::string, std::string> strings;
        for (auto it = localeDocument["strings"].begin(); it != localeDocument["strings"].end(); ++it) {
            if (!it.value().is_string()) {
                m_runtimeStartupErrorMessage = "Locale strings must contain text values";
                return false;
            }
            strings.emplace(it.key(), it.value().get<std::string>());
        }
        std::vector<std::string> fonts;
        if (const auto chain = localeDocument.find("fontChain"); chain != localeDocument.end()) {
            if (!chain->is_array() || chain->size() > 16) {
                m_runtimeStartupErrorMessage = "Locale fontChain must be a bounded array";
                return false;
            }
            std::set<std::string> unique;
            for (const auto& entry : *chain) {
                if (!entry.is_string()) return false;
                const auto font = entry.get<std::string>();
                if ((!font.ends_with(".ttf") && !font.ends_with(".otf")) ||
                    !ReadProjectFile(px::PathToUtf8(projectRoot), font) || !unique.insert(font).second) {
                    m_runtimeStartupErrorMessage = "Locale font is missing, unsafe, duplicated, or unsupported: " + font;
                    return false;
                }
                fonts.push_back(font);
            }
        }
        runtime->Renderer().SetTextLocale(locale, std::move(fonts));
        session->VM().SetCharacterLocale(locale);
        session->VM().SetTextFilter([strings = std::move(strings)](const std::string& id, const std::string& fallback) {
            const auto found = strings.find(id);
            return found == strings.end() ? fallback : found->second;
        });
        if (const auto entries = project.find("uiEntryPoints"); entries != project.end() && entries->is_object()) {
            for (auto entry = entries->begin(); entry != entries->end(); ++entry) {
                if (!entry.value().is_string()) return false;
                const auto path = entry.value().get<std::string>();
                const auto scene = ReadProjectFile(px::PathToUtf8(projectRoot), path);
                if (!scene || !m_hud.RegisterRouteTemplate(entry.key(), scene->second, path)) {
                    m_runtimeStartupErrorMessage = "Authored UI route could not be loaded: " + path;
                    return false;
                }
            }
        }
        m_characterCount = m_gameCatalog.Characters().size();
        m_characterResourcesDeclared = characterResourcesDeclared;
        spdlog::info("Preview canonical catalog loaded characters={} variables={}",
                     m_gameCatalog.Characters().size(),
                     m_gameCatalog.Variables().size());
        m_hud.SetActionSink([this](const px::ui::GalgameAction& action) {
            // Replacing a UI tree from inside Button::Activate destroys the
            // active callback owner. Apply host actions at the next frame.
            if (m_previewActions.size() < 64) m_previewActions.push_back(action);
        });
        m_projectRoot = projectRoot;
        m_runtime = std::move(runtime);
        m_session = std::move(session);
        m_hud.SetTextRenderer(&m_runtime->Renderer());
        m_uiPreview.SetTextRenderer(&m_runtime->Renderer());
        m_session->SetUIStateHandler(
            [this] { return m_uiPreview.CaptureRuntimeState(); },
            [this](const px::ui::UIRuntimeState& state) {
                return m_uiPreview.RestoreRuntimeState(state);
            });
        m_scriptServices.vfs = &m_runtime->VFS();
        m_scriptServices.renderer = &m_runtime->Renderer();
        m_scriptServices.audio = &m_runtime->Audio();
        m_scriptServices.input = &m_runtime->GetInput();
        m_scriptServices.stage = &m_session->Stage();
        m_scriptServices.variables = &m_session->Variables();
        m_scriptServices.routes = &m_session->Routes();
        m_scriptServices.timeline = &m_session->Timeline();
        m_scriptServices.loadAnimationAsset = [this](const std::string& path) {
            auto loaded = m_session->LoadAnimationAsset(path);
            if (!loaded)
                return px::Result<std::string>::Failure(loaded.Diagnostics());
            return px::Result<std::string>::Success(
                loaded.Value().ToString());
        };
        m_scriptServices.queryDialogue = [this] {
            return px::script::BuildDialogueModel(m_session->Dialogue());
        };
        m_scriptServices.presentScriptSay = [this](const px::vn::Command& command) {
            return m_session->VM().PresentScriptSay(command);
        };
        m_scriptServices.queryChoices = [this] {
            return px::script::BuildChoiceModels(m_session->VM());
        };
        m_scriptServices.queryBacklog = [this] {
            return px::script::BuildBacklogModels(m_session->Backlog());
        };
        auto previewSettings = std::make_shared<px::VariantObject>(
            px::VariantObject{
                {"bgmVolume", px::Variant(96)},
                {"seVolume", px::Variant(110)},
                {"voiceVolume", px::Variant(128)},
                {"textSpeedMs", px::Variant(28)},
                {"autoWaitMs", px::Variant(1400)},
                {"skipReadOnly", px::Variant(true)},
                {"fullscreen", px::Variant(false)},
                {"windowWidth", px::Variant(config.width)},
                {"windowHeight", px::Variant(config.height)},
                {"language", px::Variant("preview")},
                {"textScale", px::Variant(1.0)},
                {"highContrast", px::Variant(false)},
                {"reducedMotion", px::Variant(false)},
                {"selfVoicing", px::Variant(false)},
                {"supportedLocales", px::Variant(px::VariantArray{})}});
        m_scriptServices.querySettings = [previewSettings] {
            px::VariantObject copy;
            for (const auto& [name, value] : *previewSettings)
                copy.emplace(name, value.Clone());
            return px::Variant(std::move(copy));
        };
        m_scriptServices.advanceDialogue = [this] {
            if (m_session->VM().State() == px::vn::VMState::WaitingChoice)
                return false;
            m_session->Advance();
            return true;
        };
        m_scriptServices.selectChoice = [this](const int index) {
            const auto& choices = m_session->VM().Choices();
            if (m_session->VM().State() != px::vn::VMState::WaitingChoice ||
                index < 0 || static_cast<std::size_t>(index) >= choices.size())
                return false;
            m_session->SelectChoice(index);
            return true;
        };
        m_scriptServices.replayBacklogVoice =
            [this](const std::uint64_t sequence) {
                const auto* entry = m_session->Backlog().Find(sequence);
                if (!entry || entry->voice.empty()) return false;
                m_runtime->Audio().PlayVoice(entry->voice);
                return true;
            };
        m_scriptServices.rollbackBacklog = [](std::uint64_t) { return false; };
        m_scriptServices.updateSetting =
            [previewSettings](const std::string_view name,
                              const px::Variant& value) {
                auto found = previewSettings->find(std::string(name));
                if (found == previewSettings->end()) return false;
                found->second = value.Clone();
                return true;
            };
        m_scriptServices.console = [events = m_events](const px::script::ConsoleMessage& message) {
            std::string_view level = "info";
            std::string_view stream = "stdout";
            if (message.level == px::script::ConsoleLevel::Warning) {
                level = "warning";
                stream = "stderr";
            }
            else if (message.level == px::script::ConsoleLevel::Error) {
                level = "error";
                stream = "stderr";
            }
            events->Push("output", Json{ { "scope", "script" }, { "stream", stream }, { "level", level }, { "message", message.text }, { "source", message.source }, { "line", message.line } });
        };
        m_scriptHost = px::script::CreateScriptHost(m_scriptServices);
        m_session->SetDialogueAdvanceHandler([this](px::vn::Dialogue& dialogue) {
            return m_scriptHost && m_scriptHost->AdvanceDialogueWait(dialogue);
        });
        px::script::BindTimelineMarkerEvents(
            m_session->Timeline(), [this] { return m_scriptHost.get(); });
        if (const auto status = m_uiPreview.Actions().RegisterProvider(m_scriptHost->CreateActionProvider()); !status) {
            spdlog::error("Preview script Action provider registration failed: {}", StatusMessage(status, "unknown error"));
            return false;
        }
        m_session->SetExtensionCommandHandler([this](const px::vn::Command& command) {
            // Preview has no name prompt: [input] stores its default and moves on.
            if (command.type == "text_input") {
                if (!m_session) return false;
                m_session->Variables().SetValue(command.Get("variable"), px::Variant(command.Get("default")),
                    command.Get("scope") == "profile" ? px::vn::VariableScope::Profile
                                                      : px::vn::VariableScope::Session);
                return true;
            }
            if (command.type == "action" && m_scriptHost) {
                const Json payload = Json::parse(command.Get("value"), nullptr, false);
                if (payload.is_discarded() || !payload.is_object() || !payload.contains("id") || !payload["id"].is_string() || !payload.contains("arguments") || !payload["arguments"].is_object()) {
                    return false;
                }
                px::ui::ActionInvocation invocation;
                invocation.action = payload["id"].get<std::string>();
                invocation.context.sourceScene = m_session->VM().CurrentScript();
                invocation.context.preview = true;
                if (const auto* typed = command.FindTyped("arguments")) {
                    const auto* arguments = typed->AsObject();
                    if (!arguments) return false;
                    for (const auto& [name, value] : *arguments) {
                        invocation.arguments.emplace(name, value.Clone());
                    }
                } else {
                    for (auto argument = payload["arguments"].begin(); argument != payload["arguments"].end(); ++argument) {
                        auto value = JsonVariant(argument.value());
                        if (!value) return false;
                        invocation.arguments.emplace(argument.key(), std::move(*value));
                    }
                }
                const auto started = m_uiPreview.Actions().Start(std::move(invocation), { .previewSafeMode = true });
                if (!started) {
                    for (const auto& diagnostic : started.Diagnostics()) {
                        px::diag::Emit(diagnostic);
                    }
                    return false;
                }
                if (m_scriptHost->HasPendingAction()) m_session->VM().WaitExternal();
                return true;
            }
            const bool handled = m_scriptHost && m_scriptHost->InvokeCommand(command);
            if (handled && (m_scriptHost->HasPendingCommand() || m_scriptHost->HasPendingAction())) {
                m_session->VM().WaitExternal();
            }
            return handled;
        });
        // Apply() calls EnsureRuntime before StartRuntimeIrText. Register the
        // exact ordered canonical project list so Preview and packaged Player
        // compile against identical extension descriptors.
        for (const auto& manifest : previewExtensions->manifests) {
            if (!m_scriptHost->LoadExtensionManifest(manifest)) {
                return false;
            }
        }
        m_extensionBuildIdentity = previewExtensions->buildIdentity;
        if (!m_scriptBreakpoints.empty()) {
            (void)m_scriptHost->SetDebugBreakpoints(m_scriptBreakpoints);
        }
        m_previewSession = px::preview::CreatePreviewSession(
            *m_session,
            {.inspectSafety = [this](const px::vn::Command& command) {
                 return InspectOperationSafety(command);
             },
             .resize = [this](const int width, const int height,
                              const float scale) {
                 if (m_runtime && m_runtime->GetWindow().Resize(
                         std::max(1, static_cast<int>(width * scale)),
                         std::max(1, static_cast<int>(height * scale))))
                     return px::Status::Ok();
                 return PreviewSessionFailure(
                     "PXPREVIEW-NATIVE-RESIZE-001",
                     "Native Preview window could not be resized.");
             },
             .captureExternalState = [this] {
                 return CaptureExternalCheckpoint();
             },
             .restoreExternalState = [this](const auto& state) {
                 return RestoreExternalCheckpoint(state);
             },
             .validateExternalState = [](const std::shared_ptr<const void>& state) {
                 if (!state) return px::Status::Ok();
                 const auto checkpoint = std::static_pointer_cast<const ScriptCheckpoint>(state);
                 return checkpoint->captureErrors.empty() ? px::Status::Ok()
                     : px::Status::Fail(checkpoint->captureErrors);
             }});
        m_scriptHost->Emit("engine.ready");
        return true;
    }

    void Frame() {
        while (!m_previewActions.empty()) {
            const auto action = std::move(m_previewActions.front());
            m_previewActions.pop_front();
            if (action.command == "choice.select")
                (void)m_previewSession->SelectChoice(std::atoi(action.argument.c_str()));
            else if (action.command == "game.start" || action.command == "overlay.close") {
                (void)m_hud.ShowHUD(DialogueView(*m_session, m_choices));
                (void)m_previewSession->Continue();
            } else if (action.command == "save.open" || action.command == "load.open") {
                const bool save = action.command == "save.open";
                if (save) {
                    const auto captured = m_previewSession->CaptureCheckpoint();
                    if (!captured.accepted || !captured.checkpoint) {
                        m_events->Push("diagnostics", Json{{"code", "PXPREVIEW1702"},
                            {"message", "Preview checkpoint capture failed; no slot was changed"}});
                        continue;
                    }
                    m_pendingPreviewSave = captured.checkpoint->id;
                }
                (void)m_previewSession->Pause();
                std::vector<px::ui::GalgameItem> slots;
                for (int slot = 0; slot < 4; ++slot) {
                    const bool exists = m_previewSaves.contains(slot);
                    slots.push_back({.key = std::to_string(slot),
                        .title = "Preview checkpoint " + std::to_string(slot + 1),
                        .subtitle = "Session only; cleared on rebuild or close",
                        .disabled = !save && !exists,
                        .command = save ? "save.slot" : "load.slot", .argument = std::to_string(slot)});
                }
                (void)m_hud.ShowSaveLoad(save, std::move(slots));
            } else if (action.command == "save.slot" || action.command == "load.slot") {
                const int slot = std::atoi(action.argument.c_str());
                if (slot < 0 || slot >= 4) continue;
                if (action.command == "save.slot") {
                    if (m_pendingPreviewSave) m_previewSaves[slot] = *m_pendingPreviewSave;
                } else {
                    const auto found = m_previewSaves.find(slot);
                    if (found == m_previewSaves.end() || !m_previewSession->RestoreCheckpoint(found->second).accepted) {
                        m_events->Push("diagnostics", Json{{"code", "PXPREVIEW1703"},
                            {"message", "Preview checkpoint is unavailable or restore failed; it may have expired from bounded session history"}});
                        continue;
                    }
                }
                (void)m_hud.ShowHUD(DialogueView(*m_session, m_choices));
                (void)m_previewSession->Continue();
            } else {
                m_events->Push("diagnostics", Json{{"code", "PXPREVIEW1701"},
                    {"message", "This Player action is not available in safe Preview; test it in the built game"},
                    {"action", action.command}});
            }
        }
        const float delta = m_runtime->GetClock().DeltaSeconds();
        const std::uint64_t now = m_runtime->GetClock().NowMs();
        if (m_scriptHost) {
            px::script::AdvanceScriptFrame(
                *m_scriptHost, delta, {}, [this] {
                    if (m_session->VM().State() ==
                        px::vn::VMState::WaitingExternal)
                        m_session->VM().NotifyExternalDone();
                });
            if (!m_showUiPreview) m_uiPreview.Actions().Update(delta);
        }
        if (m_showUiPreview) {
            auto& input = m_runtime->GetInput();
            int width = 0;
            int height = 0;
            m_runtime->Renderer().GetLogicalSize(width, height);
            (void)m_uiPreview.Update(input, width, height, delta);
            if (m_scriptHost)
                m_scriptHost->RenderRetained(
                    px::script::RenderPhase::BeforeUI);
            m_uiPreview.Render(m_runtime->Renderer());
            if (m_scriptHost)
                m_scriptHost->RenderRetained(
                    px::script::RenderPhase::AfterUI);
            return;
        }
        const auto ticked = m_previewSession->Tick(now, delta);
        if (!ticked.accepted) {
            Json diagnostics = Json::array();
            for (const auto& diagnostic : ticked.diagnostics)
                diagnostics.push_back(PreviewDiagnosticJson(diagnostic));
            m_events->Push("diagnostics",
                           Json{{"diagnostics", std::move(diagnostics)}});
        }
        m_choices.clear();
        if (m_session->VM().State() == px::vn::VMState::WaitingChoice) {
            for (const auto& choice : m_session->VM().Choices()) m_choices.push_back(choice.text);
        }
        if (m_hud.CurrentScreen() == px::ui::GalgameUI::Screen::HUD)
            (void)m_hud.RefreshHUD(DialogueView(*m_session, m_choices));
        auto& input = m_runtime->GetInput();
        int width = 0;
        int height = 0;
        m_runtime->Renderer().GetLogicalSize(width, height);
        const bool consumed = m_hud.Update(input, width, height, delta);
        if (!consumed && m_hud.CurrentScreen() == px::ui::GalgameUI::Screen::HUD && input.LeftClick() && m_session->VM().State() != px::vn::VMState::WaitingChoice) {
            (void)m_previewSession->Advance();
        }
        if (m_scriptHost)
            m_scriptHost->RenderRetained(
                px::script::RenderPhase::BeforeStage);
        m_session->Stage().Render();
        if (m_scriptHost) {
            m_scriptHost->RenderRetained(
                px::script::RenderPhase::AfterStage);
            m_scriptHost->RenderRetained(
                px::script::RenderPhase::BeforeUI);
        }
        m_hud.Render(m_runtime->Renderer());
        if (m_scriptHost)
            m_scriptHost->RenderRetained(
                px::script::RenderPhase::AfterUI);
        (void)m_previewSession->Events();
    }

    RequestQueue& m_queue;
    std::shared_ptr<AsyncEventChannel> m_events;
    SerialStdoutWriter m_stdout;
    std::unique_ptr<px::Runtime> m_runtime;
    std::unique_ptr<px::RuntimeSession> m_session;
    std::unique_ptr<px::sdk::PreviewSession> m_previewSession;
    px::vn::GameCatalog m_gameCatalog;
    px::script::ScriptServices m_scriptServices;
    std::unique_ptr<px::script::ScriptHost> m_scriptHost;
    std::vector<px::script::DebugBreakpoint> m_scriptBreakpoints;
    px::ui::GalgameUI m_hud;
    std::deque<px::ui::GalgameAction> m_previewActions;
    std::unordered_map<int, std::uint64_t> m_previewSaves;
    std::optional<std::uint64_t> m_pendingPreviewSave;
    px::ui::ObservableViewModel m_uiPreviewViewModel;
    px::ui::UIContext m_uiPreview;
    std::filesystem::path m_projectRoot;
    PreviewResolution m_resolution;
    std::string m_extensionBuildIdentity;
    std::unordered_map<std::string, std::uint64_t> m_appliedRevisions;
    std::unordered_map<std::string, std::uint64_t> m_appliedUiRevisions;
    std::unordered_map<std::string, Json> m_appliedUiDocuments;
    std::unordered_map<std::string, std::uint64_t> m_appliedPerformanceRevisions;
    std::unordered_map<std::string, Json> m_appliedPerformanceDocuments;
    std::unordered_map<std::string, RuntimeSourceLocation> m_runtimeSources;
    std::vector<std::string> m_choices;
    std::string m_lastUiAction;
    std::string m_lastRuntimeState;
    std::string m_lastDebugState;
    std::string m_activeUiSceneId;
    std::string m_activePerformanceSceneId;
    std::string m_lastUiDebugState;
    std::uint64_t m_activeUiRevision = 0;
    std::uint64_t m_uiActionCount = 0;
    std::size_t m_characterCount = 0;
    bool m_characterResourcesDeclared = false;
    bool m_gameCatalogResourcesDeclared = false;
    std::size_t m_gameCatalogVariableCount = 0;
    std::size_t m_gameCatalogInputBindingCount = 0;
    std::size_t m_gameCatalogGalleryItemCount = 0;
    std::string m_runtimeStartupErrorCode;
    std::string m_runtimeStartupErrorMessage;
    bool m_showUiPreview = false;
    bool m_shutdown = false;
    bool m_exitEventQueued = false;
};

void ReadRequests(RequestQueue& queue) {
    std::string line;
    while (std::getline(std::cin, line)) {
        Json request = Json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            request = Json{ { "protocol", kProtocol }, { "schemaRevision", kSchemaRevision }, { "protocolVersion", kProtocolVersion }, { "sessionId", "" }, { "requestId", "" }, { "documentId", "" }, { "revision", 0 }, { "type", "invalidJson" } };
        }
        const bool shutdown = request.value("type", std::string{}) == "shutdown";
        {
            std::lock_guard lock(queue.mutex);
            queue.requests.push_back(std::move(request));
        }
        queue.condition.notify_one();
        if (shutdown) break;
    }
    {
        std::lock_guard lock(queue.mutex);
        queue.inputClosed = true;
        queue.readerFinished = true;
    }
    queue.condition.notify_one();
}

// The protocol client owns PreviewHost through stdin. Killing the client
// normally closes that pipe, but a copy of its write end can outlive the
// client in another process (an inherited handle), which left Preview windows
// open after `prismatix dev` died. Treat the parent's exit as end of input too.
std::unique_ptr<px::platform::ProcessExitWatch> WatchParentProcess(RequestQueue& queue) {
    const auto parent = px::platform::ParentProcessId();
    if (!parent) return nullptr;
    return px::platform::ProcessExitWatch::Start(
        px::platform::ProcessAliveProbe(*parent), [&queue] {
            {
                std::lock_guard lock(queue.mutex);
                queue.inputClosed = true;
                queue.parentExited = true;
            }
            queue.condition.notify_one();
        });
}

}  // namespace

int main() {
    auto events = std::make_shared<AsyncEventChannel>();
    auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto outputSink = std::make_shared<RuntimeOutputSink>(events);
    outputSink->set_pattern("%v");
    std::vector<spdlog::sink_ptr> sinks{ stderrSink, outputSink };
    auto logger = std::make_shared<spdlog::logger>("PrismatiXPreviewHost", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));

    RequestQueue queue;
    std::thread reader([&queue] { ReadRequests(queue); });
    auto parentWatch = WatchParentProcess(queue);
    PreviewHost host(queue, std::move(events));
    const int result = host.Run();
    spdlog::shutdown();
    // After the window closes the client may still hold stdin open; stay (with
    // no window) until it closes the pipe or itself goes away, as before.
    bool readerFinished = false;
    {
        std::unique_lock lock(queue.mutex);
        queue.condition.wait(lock, [&queue] {
            return queue.readerFinished || queue.parentExited;
        });
        readerFinished = queue.readerFinished;
    }
    parentWatch.reset();
    if (readerFinished) {
        reader.join();
        return result;
    }
    // The parent is gone but another process still holds the stdin pipe, so
    // the reader never returns. Joining it, or exit(), which flushes stdin
    // under the lock the blocked reader holds, would keep this process alive.
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(result);
}
