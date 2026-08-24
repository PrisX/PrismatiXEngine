#include "Engine/Preview/PreviewProtocolV2.h"
#include "Engine/Preview/LocalizationPreview.h"
#include "Engine/Preview/PerformancePreview.h"
#include "Engine/Preview/ProgressionPreview.h"
#include "Engine/Preview/PreviewSessionFactory.h"

#include "Engine/Runtime.h"
#include "Engine/Script/ScriptHost.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/Ui.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/UI/Actions/BuiltInActionProvider.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/VN/GameCatalog.h"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <SDL3/SDL_hints.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

using Json = nlohmann::json;

std::string_view StateName(const px::vn::VMState state) {
    switch (state) {
        case px::vn::VMState::Idle: return "idle";
        case px::vn::VMState::Running: return "running";
        case px::vn::VMState::WaitingClick: return "waitingClick";
        case px::vn::VMState::WaitingChoice: return "waitingChoice";
        case px::vn::VMState::WaitingTimer: return "waitingTimer";
        case px::vn::VMState::WaitingVideo: return "waitingVideo";
        case px::vn::VMState::WaitingExternal: return "waitingExternal";
        case px::vn::VMState::Paused: return "paused";
        case px::vn::VMState::Finished: return "finished";
    }
    return "unknown";
}

std::string_view PreviewStatusName(const px::sdk::PreviewSessionStatus status) {
    switch (status) {
        case px::sdk::PreviewSessionStatus::Applied: return "applied";
        case px::sdk::PreviewSessionStatus::Patched: return "patched";
        case px::sdk::PreviewSessionStatus::Restarted: return "restarted";
        case px::sdk::PreviewSessionStatus::Running: return "running";
        case px::sdk::PreviewSessionStatus::Paused: return "paused";
        case px::sdk::PreviewSessionStatus::Advanced: return "advanced";
        case px::sdk::PreviewSessionStatus::ChoiceSelected: return "choiceSelected";
        case px::sdk::PreviewSessionStatus::StorySeeked: return "storySeeked";
        case px::sdk::PreviewSessionStatus::TimelineSeeked: return "timelineSeeked";
        case px::sdk::PreviewSessionStatus::CheckpointCaptured: return "checkpointCaptured";
        case px::sdk::PreviewSessionStatus::CheckpointRestored: return "checkpointRestored";
        case px::sdk::PreviewSessionStatus::Resized: return "resized";
        case px::sdk::PreviewSessionStatus::Ticked: return "ticked";
        case px::sdk::PreviewSessionStatus::InvalidArgument: return "invalidArgument";
        case px::sdk::PreviewSessionStatus::NotReady: return "notReady";
        case px::sdk::PreviewSessionStatus::RevisionConflict: return "revisionConflict";
        case px::sdk::PreviewSessionStatus::RuntimeRejected: return "runtimeRejected";
        case px::sdk::PreviewSessionStatus::ChoicePathRequired: return "choicePathRequired";
        case px::sdk::PreviewSessionStatus::UnsafeOperation: return "unsafeOperation";
        case px::sdk::PreviewSessionStatus::UnsupportedAsync: return "unsupportedAsync";
        case px::sdk::PreviewSessionStatus::UnknownCheckpoint: return "unknownCheckpoint";
        case px::sdk::PreviewSessionStatus::TimelineRejected: return "timelineRejected";
        case px::sdk::PreviewSessionStatus::Finished: return "finished";
    }
    return "unknown";
}

std::string_view PreviewSeverityName(
    const px::sdk::PreviewDiagnosticSeverity severity) {
    switch (severity) {
        case px::sdk::PreviewDiagnosticSeverity::Info: return "info";
        case px::sdk::PreviewDiagnosticSeverity::Warning: return "warning";
        case px::sdk::PreviewDiagnosticSeverity::Error: return "error";
        case px::sdk::PreviewDiagnosticSeverity::Fatal: return "fatal";
    }
    return "error";
}

Json PreviewDiagnosticJson(
    const px::sdk::PreviewSessionDiagnostic& diagnostic,
    const std::string_view fallbackDocumentId = {}) {
    const std::string documentId = diagnostic.source.resourceId.empty()
        ? std::string(fallbackDocumentId)
        : diagnostic.source.resourceId;
    return {{"severity", PreviewSeverityName(diagnostic.severity)},
            {"code", diagnostic.code},
            {"category", diagnostic.category},
            {"message", diagnostic.message},
            {"details", diagnostic.details},
            {"source",
             {{"documentId", documentId},
              {"resourceId", diagnostic.source.resourceId},
              {"path", diagnostic.source.path},
              {"blockId", diagnostic.source.nodeId},
              {"nodeId", diagnostic.source.nodeId},
              {"property", diagnostic.source.property},
              {"line", diagnostic.source.line},
              {"column", diagnostic.source.column},
              {"operation", diagnostic.operationIndex}}},
            {"operationId", diagnostic.operationId},
            {"quickFix", diagnostic.quickFix}};
}

px::ui::DialoguePresentation DialogueView(
    const px::RuntimeSession& session,
    const std::vector<std::string>& choices) {
    const auto& dialogue = session.Dialogue().State();
    px::ui::DialoguePresentation view;
    view.speaker = dialogue.speaker;
    view.text = dialogue.displayText;
    view.chapterTitle = session.VM().Chapter();
    view.musicTitle = session.VM().CurrentBgm();
    view.choices = choices;
    view.effect = dialogue.effect;
    view.effectProgress = dialogue.effectProgress;
    return view;
}

class WasmPreview final {
public:
    explicit WasmPreview(const char* optionsUtf8) {
        Json options = Json::parse(optionsUtf8 ? optionsUtf8 : "{}", nullptr,
                                   false);
        if (options.is_discarded() || !options.is_object()) options = Json::object();
        px::RuntimeConfig config;
        const std::string canvasSelector =
            options.value("canvasSelector", std::string("#canvas"));
        const bool validCanvasSelector =
            canvasSelector.size() >= 2 && canvasSelector.size() <= 129 &&
            canvasSelector.front() == '#' &&
            std::isalpha(static_cast<unsigned char>(canvasSelector[1])) &&
            std::all_of(canvasSelector.begin() + 2, canvasSelector.end(),
                        [](const unsigned char value) {
                            return std::isalnum(value) || value == '_' ||
                                   value == '-';
                        });
        if (!validCanvasSelector) {
            m_protocol.Emit(
                "crashed",
                Json{{"code", "PXWASM-CANVAS-001"},
                     {"message", "Preview canvas selector is invalid."},
                     {"recoverable", true}}
                    .dump());
            return;
        }
#if defined(__EMSCRIPTEN__)
        SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR,
                    canvasSelector.c_str());
        SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT,
                    canvasSelector.c_str());
#endif
        config.title = "PrismatiX Studio Preview";
        config.width = std::max(1, options.value("width", 1280));
        config.height = std::max(1, options.value("height", 720));
        config.logicalWidth = std::max(1, options.value("logicalWidth", 1280));
        config.logicalHeight = std::max(1, options.value("logicalHeight", 720));
        config.resizable = true;
        config.asyncAssetPreload = false;
        config.mountDirs = {"/project"};
        config.mountArchives.clear();
        if (!m_runtime.Init(config)) {
            m_protocol.Emit(
                "crashed",
                Json{{"code", "PXWASM-STARTUP-001"},
                     {"message", "SDL WASM preview runtime could not initialize."},
                     {"recoverable", true}}
                    .dump());
            return;
        }
        m_session = std::make_unique<px::RuntimeSession>(
            px::RuntimeSession::Services{m_runtime.VFS(), m_runtime.Audio(),
                                         m_runtime.Renderer(),
                                         m_runtime.Assets()});
        m_previewSession = px::preview::CreatePreviewSession(
            *m_session,
            {.inspectSafety = [this](const px::vn::Command& command) {
                 return InspectOperationSafety(command);
             },
             .resize = [this](const int width, const int height,
                              const float scale) {
                 const int pixelWidth = std::max(
                     1, static_cast<int>(static_cast<float>(width) * scale));
                 const int pixelHeight = std::max(
                     1, static_cast<int>(static_cast<float>(height) * scale));
                 if (m_runtime.GetWindow().Resize(pixelWidth, pixelHeight))
                     return px::Status::Ok();
                 px::diag::Diagnostic diagnostic;
                 diagnostic.severity = px::diag::Severity::Error;
                 diagnostic.code = "PXWASM-RESIZE-001";
                 diagnostic.category = "Preview.Viewport";
                 diagnostic.message = "Preview canvas could not be resized.";
                 return px::Status::Fail(std::move(diagnostic));
             },
             .captureExternalState = [this] {
                 return CaptureExternalCheckpoint();
             },
             .restoreExternalState = [this](const auto& state) {
                 return RestoreExternalCheckpoint(state);
             }});
        m_hud.SetBehaviorVariableAccess(
            [this](const std::string_view name) -> std::optional<px::Variant> {
                const auto* value = m_session->Variables().GetValue(name);
                return value ? std::optional<px::Variant>{value->Clone()}
                             : std::nullopt;
            },
            [this](const std::string_view name, const px::Variant& value) {
                m_session->Variables().SetValue(
                    std::string(name), value.Clone(),
                    px::vn::VariableScope::SaveLocal);
                return px::Status::Ok();
            });
        m_hud.SetExternalAnimationServices(
            [this](const std::string_view path) {
                return m_session->PlayAnimationAsset(std::string(path));
            },
            [this](const std::uint64_t handle) {
                return m_session->Timeline().Playing(handle);
            });
        m_session->SetBehaviorStateHandler(
            [this] { return m_hud.CaptureBehaviorState(); },
            [this](const px::ui::BehaviorRuntimeState& state) {
                return m_hud.RestoreBehaviorState(state);
            });
        m_session->SetAnimationTargetHandler(
            px::animation::TargetKind::UI,
            [this](const auto& binding, const px::Variant& value) {
                return m_hud.ApplyAnimationProperty(binding, value);
            });
        m_session->SetAnimationTargetHandler(
            px::animation::TargetKind::Text,
            [this](const auto& binding, const px::Variant& value) {
                return m_hud.ApplyAnimationProperty(binding, value);
            });
        for (const std::string_view route :
             {"title", "hud", "game", "save", "load", "saveload",
              "settings", "backlog", "gallery"})
            (void)EnsurePreviewRoute(route);
        m_session->SetRoutePresentationHandler(
            [this](const std::string_view route,
                   const std::string_view operation) {
                PresentPreviewRoute(route, operation);
            });
        m_hud.SetActionSink([this](const px::ui::GalgameAction& action) {
            const auto status = ExecuteUiAction(action.command, action.argument);
            if (!status)
                EmitStatusDiagnostics(status, "PXWASM-UI-ACTION-001");
        });
        const auto previewAction = [this](
                                       const px::ui::ActionInvocation& invocation) {
            m_lastUiAction = invocation.action;
            const auto status = ExecuteUiAction(invocation);
            if (!status)
                EmitStatusDiagnostics(status, "PXWASM-UI-ACTION-001");
            return status;
        };
        px::Status actionProviderStatus = px::Status::Ok();
        if (auto existing = std::dynamic_pointer_cast<
                px::ui::BuiltInActionProvider>(
                m_hud.Actions().FindProvider("builtin"))) {
            existing->SetFallback(previewAction);
        } else {
            auto actionProvider =
                std::make_shared<px::ui::BuiltInActionProvider>();
            actionProvider->SetFallback(previewAction);
            actionProviderStatus =
                m_hud.Actions().RegisterProvider(std::move(actionProvider));
        }
        if (!actionProviderStatus) {
            m_protocol.Emit(
                "crashed",
                Json{{"code", "PXWASM-UI-ACTIONS-001"},
                     {"message", "Runtime UI Action provider could not initialize."},
                     {"recoverable", true}}
                    .dump());
            return;
        }
        m_scriptServices.vfs = &m_runtime.VFS();
        m_scriptServices.renderer = &m_runtime.Renderer();
        m_scriptServices.audio = &m_runtime.Audio();
        m_scriptServices.input = &m_runtime.GetInput();
        m_scriptServices.stage = &m_session->Stage();
        m_scriptServices.variables = &m_session->Variables();
        m_scriptServices.routes = &m_session->Routes();
        m_scriptServices.timeline = &m_session->Timeline();
        m_scriptServices.console = [this](
                                    const px::script::ConsoleMessage& message) {
            std::string_view level = "info";
            std::string_view stream = "stdout";
            if (message.level == px::script::ConsoleLevel::Warning) {
                level = "warning";
                stream = "stderr";
            } else if (message.level == px::script::ConsoleLevel::Error) {
                level = "error";
                stream = "stderr";
            }
            m_protocol.Emit(
                "output",
                Json{{"scope", "script"},
                     {"stream", stream},
                     {"level", level},
                     {"message", message.text},
                     {"source", message.source},
                     {"line", message.line}}
                    .dump());
        };
        m_scriptHost = px::script::CreateScriptHost(m_scriptServices);
        if (const auto status =
                m_hud.Actions().RegisterProvider(m_scriptHost->CreateActionProvider());
            !status) {
            m_protocol.Emit(
                "crashed",
                Json{{"code", "PXWASM-SCRIPT-STARTUP-001"},
                     {"message", "WASM script Action provider could not initialize."},
                     {"recoverable", true}}
                    .dump());
            return;
        }
        m_session->SetExtensionCommandHandler(
            [this](const px::vn::Command& command) {
                if (!m_scriptHost || !m_session) return false;
                if (command.type == "action") {
                    const Json payload =
                        Json::parse(command.Get("value"), nullptr, false);
                    if (payload.is_discarded() || !payload.is_object() ||
                        !payload.contains("id") || !payload["id"].is_string())
                        return false;
                    px::ui::ActionInvocation invocation;
                    invocation.action = payload["id"].get<std::string>();
                    invocation.context.sourceScene =
                        m_session->VM().CurrentScript();
                    invocation.context.preview = true;
                    if (const auto* typed = command.FindTyped("arguments")) {
                        const auto* arguments = typed->AsObject();
                        if (!arguments) return false;
                        for (const auto& [name, value] : *arguments)
                            invocation.arguments.emplace(name, value.Clone());
                    }
                    const auto started = m_hud.Actions().Start(
                        std::move(invocation), {.previewSafeMode = true});
                    if (!started) {
                        EmitStatusDiagnostics(started, "PXWASM-SCRIPT-ACTION-001");
                        return false;
                    }
                    if (m_scriptHost->HasPendingAction())
                        m_session->VM().WaitExternal();
                    return true;
                }
                const bool handled = m_scriptHost->InvokeCommand(command);
                if (handled && (m_scriptHost->HasPendingCommand() ||
                                m_scriptHost->HasPendingAction()))
                    m_session->VM().WaitExternal();
                return handled;
            });
        m_ready = true;
        m_protocol.Emit("state",
                        Json{{"status", "ready"}, {"mode", "wasm"}}.dump());
    }

    int Apply(const char* envelopeUtf8, const bool patch) {
        if (!envelopeUtf8) return 0;
        auto accepted = m_protocol.AcceptApply(envelopeUtf8, patch);
        if (!accepted.Accepted() || !m_ready || !m_session) return 0;
        const auto& request = *accepted.request;
        const auto runtimeIrContract = px::sdk::ParseRuntimeIr(request.runtimeIr);
        if (runtimeIrContract.Valid()) {
            for (const auto& operation : runtimeIrContract.document.operations) {
                if (operation.kind != "route") continue;
                const auto route = operation.arguments.find("route");
                if (route != operation.arguments.end())
                    (void)EnsurePreviewRoute(route->second);
            }
        }
        px::vn::GameCatalog catalog;
        bool characterResourcesDeclared = false;
        const auto projectManifest = m_runtime.VFS().ReadText("project.pxproject");
        if (!projectManifest) {
            m_protocol.Emit(
                "diagnostics",
                Json{{"diagnostics",
                      Json::array({{{"severity", "error"},
                                    {"code", "PXWASM-ASSET-002"},
                                    {"category", "Preview.Assets"},
                                    {"message", "Preview asset manifest is missing from MEMFS."},
                                    {"source", {{"documentId", request.documentId}}}}})}}
                    .dump());
            return 0;
        }
        const auto characterStatus = catalog.LoadCharacterResources(
            *projectManifest,
            [this](const std::string_view uri) {
                return m_runtime.VFS().ReadText(std::string(uri));
            },
            [this](const std::string_view uri) {
                return m_runtime.VFS().Exists(std::string(uri));
            },
            characterResourcesDeclared);
        if (!characterStatus) {
            Json diagnostics = Json::array();
            for (const auto& diagnostic : characterStatus.Diagnostics()) {
                diagnostics.push_back(
                    {{"severity", "error"},
                     {"code", diagnostic.code},
                     {"category", diagnostic.category},
                     {"message", diagnostic.message},
                     {"details", diagnostic.details},
                     {"source",
                      {{"documentId", request.documentId},
                       {"blockId", diagnostic.source.nodeId},
                       {"path", diagnostic.source.path},
                       {"line", diagnostic.source.line}}}});
            }
            m_protocol.Emit("diagnostics",
                            Json{{"diagnostics", diagnostics}}.dump());
            return 0;
        }
        m_session->VM().SetGameCatalog(catalog);
        if (!InstallRuntimeFiles(request.runtimeFilesJson)) return 0;
        std::optional<px::preview::LocalizationPreviewTable>
            localizationTable;
        if (!request.localizationJson.empty()) {
            auto localized = px::preview::BuildLocalizationPreviewTable(
                request.localizationJson);
            if (!localized) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic : localized.Diagnostics()) {
                    diagnostics.push_back(
                        {{"severity", px::diag::ToString(diagnostic.severity)},
                         {"code", diagnostic.code},
                         {"category", diagnostic.category},
                         {"message", diagnostic.message},
                         {"details", diagnostic.details},
                         {"source",
                          {{"documentId", diagnostic.source.resourceId},
                           {"blockId", diagnostic.source.nodeId},
                           {"path", diagnostic.source.path},
                           {"property", diagnostic.source.property},
                           {"line", diagnostic.source.line}}}});
                }
                m_protocol.Emit("diagnostics",
                                Json{{"diagnostics", diagnostics}}.dump());
                return 0;
            }
            localizationTable = localized.TakeValue();
            if (!localizationTable->diagnostics.empty()) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic :
                     localizationTable->diagnostics) {
                    diagnostics.push_back(
                        {{"severity", px::diag::ToString(diagnostic.severity)},
                         {"code", diagnostic.code},
                         {"category", diagnostic.category},
                         {"message", diagnostic.message},
                         {"details", diagnostic.details},
                         {"source",
                          {{"documentId", diagnostic.source.resourceId},
                           {"blockId", diagnostic.source.nodeId},
                           {"path", diagnostic.source.path},
                           {"property", diagnostic.source.property},
                           {"line", diagnostic.source.line}}}});
                }
                m_protocol.Emit("diagnostics",
                                Json{{"diagnostics", diagnostics}}.dump());
            }
            m_session->VM().SetTextFilter(
                [entries = localizationTable->entries](
                    const std::string& sourceId,
                    const std::string& fallback) {
                    const auto found = entries.find(sourceId);
                    return found == entries.end() ? fallback
                                                  : found->second.text;
                });
        } else {
            m_session->VM().SetTextFilter({});
        }
        const bool uiPreviewRequested = !request.uiSceneJson.empty();
        std::optional<px::preview::PerformancePreviewSequence>
            performanceSequence;
        std::optional<px::preview::PerformancePreviewPlan> performancePlan;
        if (!request.performanceJson.empty()) {
            auto compiled = px::preview::BuildPerformancePreviewSequence(
                request.performanceJson, *projectManifest, catalog,
                request.documentId,
                [this](const std::string_view uri) {
                    return m_runtime.VFS().Exists(std::string(uri));
                });
            if (!compiled) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic : compiled.Diagnostics()) {
                    diagnostics.push_back(
                        {{"severity", "error"},
                         {"code", diagnostic.code},
                         {"category", diagnostic.category},
                         {"message", diagnostic.message},
                         {"details", diagnostic.details},
                         {"source",
                          {{"documentId", request.documentId},
                           {"blockId", diagnostic.source.nodeId},
                           {"path", diagnostic.source.path},
                           {"property", diagnostic.source.property},
                           {"line", diagnostic.source.line}}}});
                }
                m_protocol.Emit("diagnostics",
                                Json{{"diagnostics", diagnostics}}.dump());
                return 0;
            }
            performanceSequence = compiled.TakeValue();
            if (!performanceSequence->uiSceneId.empty() &&
                !uiPreviewRequested) {
                m_protocol.Emit(
                    "diagnostics",
                    Json{{"diagnostics",
                          Json::array({{{"severity", "error"},
                                        {"code", "PXWASM-PERFORMANCE-UI-001"},
                                        {"category", "Preview.Performance"},
                                        {"message", "Timeline UI clips require a bound authored UI scene."},
                                        {"capability", "timeline.ui.scene"},
                                        {"nativeCheckAvailable", true},
                                        {"suggestion", "Bind a UI scene in Stage, or remove the UI track before Preview."},
                                        {"source", {{"documentId", request.documentId}, {"property", "stage.uiSceneId"}}}}})}}
                        .dump());
                return 0;
            }
            for (const auto& event : performanceSequence->events) {
                const auto* descriptor =
                    m_hud.Actions().Catalog().Find(event.actionId);
                px::ui::ActionInvocation invocation;
                invocation.action = event.actionId;
                invocation.arguments = event.arguments;
                invocation.context.sourceScene =
                    "performance://" + performanceSequence->sceneId;
                invocation.context.sourceNode =
                    px::Uuid::Parse(event.id).value_or(px::Uuid::FromName(
                        "PrismatiX.Performance.Event/" + event.id));
                invocation.context.signal = "timeline.event";
                invocation.context.preview = true;
                bool argumentsValid = false;
                if (descriptor) {
                    const auto normalized = m_hud.Actions()
                                                .Catalog()
                                                .ValidateAndNormalize(invocation);
                    argumentsValid = static_cast<bool>(normalized);
                }
                if (descriptor && descriptor->available &&
                    !descriptor->destructiveInPreview &&
                    descriptor->previewSafe && descriptor->deterministic &&
                    argumentsValid)
                    continue;
                const std::string message = !descriptor
                    ? "Timeline event references an Action that is absent from the pinned Runtime catalog."
                    : !descriptor->available
                        ? "Timeline event Action is unavailable: " +
                              descriptor->unavailableReason
                        : descriptor->destructiveInPreview ||
                                  !descriptor->previewSafe ||
                                  !descriptor->deterministic
                            ? "Timeline event Action is blocked by Preview Safe Mode. Run Native Check to verify it."
                            : "Timeline event arguments do not satisfy the typed Action contract.";
                m_protocol.Emit(
                    "diagnostics",
                    Json{{"diagnostics",
                          Json::array({{{"severity", "error"},
                                        {"code", "PXWASM-PERFORMANCE-EVENT-001"},
                                        {"category", "Preview.Performance"},
                                        {"message", message},
                                        {"capability", "timeline.event.action"},
                                        {"nativeCheckAvailable", true},
                                        {"suggestion", "Choose an available, preview-safe Action with valid typed arguments, or use Ship > Native Check."},
                                        {"source", {{"documentId", request.documentId}, {"nodeId", event.id}, {"property", "payload.actionId"}}}}})}}
                        .dump());
                return 0;
            }
            performancePlan = performanceSequence->Sample(0.0);
        }
        Json uiScene = Json::parse(request.uiSceneJson, nullptr, false);
        std::unordered_map<std::string, px::ui::UiComponentSource>
            uiComponents;
        if (!request.uiSceneJson.empty()) {
            const auto parsedUi = px::sdk::ParseUi(request.uiSceneJson);
            if (!parsedUi.Valid()) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic : parsedUi.diagnostics) {
                    diagnostics.push_back(
                        {{"severity", "error"},
                         {"code", diagnostic.code},
                         {"category", "SDK.UI.Contract"},
                         {"message", diagnostic.message},
                         {"source",
                          {{"documentId", uiScene.value("id", std::string{})},
                           {"nodeIndex", diagnostic.nodeIndex}}}});
                }
                m_protocol.Emit("diagnostics",
                                Json{{"diagnostics", diagnostics}}.dump());
                return 0;
            }
            if (performanceSequence &&
                !performanceSequence->uiSceneId.empty() &&
                parsedUi.document.id != performanceSequence->uiSceneId) {
                m_protocol.Emit(
                    "diagnostics",
                    Json{{"diagnostics",
                          Json::array({{{"severity", "error"},
                                        {"code", "PXWASM-PERFORMANCE-UI-002"},
                                        {"category", "Preview.Performance"},
                                        {"message", "The applied UI scene does not match the Performance Stage binding."},
                                        {"capability", "timeline.ui.scene"},
                                        {"nativeCheckAvailable", true},
                                        {"suggestion", "Reapply the UI scene selected by stage.uiSceneId."},
                                        {"source", {{"documentId", request.documentId}, {"property", "stage.uiSceneId"}}}}})}}
                        .dump());
                return 0;
            }
            const Json components = Json::parse(request.uiComponentsJson,
                                                nullptr, false);
            if (components.is_discarded() || !components.is_array()) return 0;
            for (const auto& component : components) {
                if (!component.is_object() ||
                    !component.contains("id") ||
                    !component["id"].is_string() ||
                    !component.contains("document") ||
                    !component["document"].is_object()) {
                    m_protocol.Emit(
                        "diagnostics",
                        Json{{"diagnostics",
                              Json::array({{{"severity", "error"},
                                            {"code", "PXWASM-UI-COMPONENT-001"},
                                            {"category", "Preview.UI"},
                                            {"message", "Preview UI component payload is invalid."}}})}}
                            .dump());
                    return 0;
                }
                const std::string id = component["id"].get<std::string>();
                const std::string sourcePath = component.value(
                    "uri", "memory://preview/components/" + id +
                               ".pxuicomponent");
                const std::string json = component["document"].dump();
                const auto parsedComponent =
                    px::sdk::ParseUiComponent(json);
                if (!parsedComponent.Valid() ||
                    parsedComponent.document.content.id != id) {
                    m_protocol.Emit(
                        "diagnostics",
                        Json{{"diagnostics",
                              Json::array({{{"severity", "error"},
                                            {"code", "PXWASM-UI-COMPONENT-002"},
                                            {"category", "Preview.UI"},
                                            {"message", "Preview UI component identity or contract is invalid."},
                                            {"source", {{"documentId", id}, {"path", sourcePath}}}}})}}
                            .dump());
                    return 0;
                }
                uiComponents.emplace(
                    id, px::ui::UiComponentSource{sourcePath, json});
            }
        }
        constexpr std::string_view runtimeIrPath =
            "memory://preview/runtime-ir.json";
        const px::sdk::PreviewApplyRequest previewRequest{
            request.documentId, request.revision, request.runtimeIr,
            std::string(runtimeIrPath)};
        const px::sdk::PreviewApplyResult previewApply =
            patch ? m_previewSession->Patch(previewRequest)
                  : m_previewSession->Apply(previewRequest);
        const bool runtimeInstalled = previewApply.accepted;
        if (!runtimeInstalled) {
            Json diagnostics = Json::array();
            for (const auto& diagnostic : previewApply.diagnostics) {
                diagnostics.push_back(
                    PreviewDiagnosticJson(diagnostic, request.documentId));
            }
            m_protocol.Emit("diagnostics",
                            Json{{"diagnostics", diagnostics}}.dump());
            return 0;
        }
        const bool performanceChanged =
            performancePlan &&
            (!patch || !previewApply.inPlace ||
             performancePlan->revision != m_performanceRevision);
        const bool performanceRemoved =
            !performancePlan && m_performanceSequence.has_value();
        if ((performanceChanged || performanceRemoved) &&
            m_performanceSequence)
            StopPerformanceAudio();
        if (performanceChanged) {
            px::preview::ApplyPerformancePreviewPlan(*m_session,
                                                     *performancePlan);
            m_performanceAudioSignature = performancePlan->audio.signature;
        }
        m_performanceRevision =
            performancePlan ? performancePlan->revision : 0;
        m_choices.clear();
        if (uiPreviewRequested) {
            std::unordered_map<std::string, std::string> uiAssets;
            const Json manifest = Json::parse(*projectManifest, nullptr, false);
            if (!manifest.is_discarded() && manifest.is_object() &&
                manifest.contains("assets") && manifest["assets"].is_array()) {
                for (const auto& asset : manifest["assets"]) {
                    if (asset.is_object() && asset.contains("id") &&
                        asset["id"].is_string() && asset.contains("source") &&
                        asset["source"].is_string())
                        uiAssets.emplace(asset["id"].get<std::string>(),
                                         asset["source"].get<std::string>());
                }
            }
            m_hud.SetUiAssetResolver(
                [assets = std::move(uiAssets)](
                    const std::string_view id) -> std::optional<std::string> {
                    const auto found = assets.find(std::string(id));
                    return found == assets.end()
                               ? std::nullopt
                               : std::optional<std::string>{found->second};
                });
            m_hud.SetUiComponentLoader(
                [components = std::move(uiComponents)](
                    const std::string_view id)
                    -> std::optional<px::ui::UiComponentSource> {
                    const auto found = components.find(std::string(id));
                    return found == components.end()
                               ? std::nullopt
                               : std::optional<px::ui::UiComponentSource>{
                                     found->second};
                });
            const std::string uiSource =
                "memory://preview/ui/" +
                uiScene.value("id", std::string{"scene"}) + ".pxui";
            const auto registered = m_hud.RegisterTemplate(
                px::ui::GalgameUI::Screen::Title, request.uiSceneJson,
                uiSource);
            const auto shown = registered ? m_hud.ShowTitle() : registered;
            if (!shown) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic : shown.Diagnostics()) {
                    diagnostics.push_back(
                        {{"severity", "error"},
                         {"code", diagnostic.code},
                         {"category", diagnostic.category},
                         {"message", diagnostic.message},
                         {"details", diagnostic.details},
                         {"source",
                          {{"documentId", uiScene.value("id", std::string{})},
                           {"blockId", diagnostic.source.nodeId},
                           {"path", diagnostic.source.path},
                           {"property", diagnostic.source.property},
                           {"line", diagnostic.source.line}}}});
                }
                m_protocol.Emit("diagnostics",
                                Json{{"diagnostics", diagnostics}}.dump());
                return 0;
            }
        } else {
            (void)m_hud.ShowHUD(CurrentDialogueView());
        }
        if (performancePlan && !ApplyPerformanceUi(*performancePlan)) return 0;
        m_protocol.CommitApply(request);
        m_catalog = catalog;
        m_projectManifestJson = *projectManifest;
        m_runtimeIrJson = request.runtimeIr;
        m_performanceJson = request.performanceJson;
        m_performanceSequence = std::move(performanceSequence);
        m_performanceTime = 0.0;
        m_lastPerformanceEventTime = 0.0;
        m_uiPreviewActive = uiPreviewRequested;
        m_paused = false;
        m_operationFocusOverride.reset();
        m_lastFocus.clear();
        m_lastVmState.clear();
        m_protocol.Emit(
            "state",
            Json{{"status", "running"},
                 {"mode", "wasm"},
                 {"characterCount", catalog.Characters().size()},
                 {"characterResourcesRevision",
                  characterResourcesDeclared
                      ? px::sdk::kCharacterResourcesContractRevision
                      : 0},
                 {"localizationRevision",
                  localizationTable ? localizationTable->documentRevision : 0},
                 {"locale",
                  localizationTable ? localizationTable->locale
                                    : std::string{}},
                 {"pseudoLocale",
                  localizationTable ? localizationTable->pseudo : false},
                 {"localizationFocusSourceId",
                  localizationTable ? localizationTable->focusSourceId
                                    : std::string{}},
                 {"performanceRevision",
                  performancePlan ? performancePlan->revision : 0},
                 {"performanceNodeCount",
                  performancePlan ? performancePlan->nodes.size() : 0},
                 {"uiSceneId",
                  m_uiPreviewActive
                      ? uiScene.value("id", std::string{})
                      : std::string{}},
                 {"uiRevision",
                  m_uiPreviewActive ? uiScene.value("revision", 0ULL) : 0ULL},
                 {"uiNodeCount",
                  m_uiPreviewActive && uiScene.contains("nodes") &&
                          uiScene["nodes"].is_array()
                      ? uiScene["nodes"].size()
                      : 0},
                 {"reloadPlan",
                  !patch ? "fullApply"
                         : previewApply.inPlace ? "inPlaceRuntimeIrPatch"
                                                : "structuralRuntimeRestart"},
                 {"patchFallbackReason",
                  patch && !previewApply.inPlace
                      ? std::string(PreviewStatusName(previewApply.status))
                      : std::string{}},
                 {"appliedRevision", request.revision}}
                .dump());
        (void)m_previewSession->Events();
        return 1;
    }

    int Control(const char* envelopeUtf8) {
        if (!m_session) return 0;
        const auto accepted =
            m_protocol.AcceptControl(envelopeUtf8 ? envelopeUtf8 : "");
        if (!accepted.Accepted()) return 0;
        const auto& request = *accepted.request;
        const Json payload = Json::parse(request.payloadJson);
        const std::string& command = request.command;
        if (command == "play" || command == "continue") {
            m_operationFocusOverride.reset();
            const bool restarting = command == "play" &&
                m_session->VM().State() == px::vn::VMState::Finished;
            const auto previewResult = command == "play"
                ? m_previewSession->Play()
                : m_previewSession->Continue();
            if (!previewResult.accepted) {
                EmitPreviewDiagnostics(previewResult, "PXWASM-PLAY-001");
                return 0;
            }
            if (restarting) {
                m_choices.clear();
                m_autoTimerStartedAtMs = 0;
                m_lastFocus.clear();
                m_lastVmState.clear();
                if (!m_uiPreviewActive)
                    (void)m_hud.ShowHUD(CurrentDialogueView());
            }
            if (command == "play" && m_performanceSequence &&
                m_performanceTime >= m_performanceSequence->duration) {
                m_performanceTime = 0.0;
                m_lastPerformanceEventTime = 0.0;
                const auto plan = m_performanceSequence->Sample(0.0);
                px::preview::ApplyPerformancePreviewPlan(
                    *m_session, plan);
                if (!ApplyPerformanceUi(plan)) return 0;
                m_performanceAudioSignature = plan.audio.signature;
            }
            m_paused = false;
            SetPerformanceAudioPaused(false);
            m_protocol.Emit("audioState",
                            Json{{"status", "unlockRequested"}}.dump());
        } else if (command == "pause") {
            const auto previewResult = m_previewSession->Pause();
            if (!previewResult.accepted && !m_performanceSequence) {
                EmitPreviewDiagnostics(previewResult, "PXWASM-DEBUG-001");
                return 0;
            }
            m_paused = true;
            SetPerformanceAudioPaused(true);
        } else if (command == "step") {
            m_operationFocusOverride.reset();
            if (m_session->VM().State() != px::vn::VMState::Paused ||
                m_session->VM().ManuallyPaused()) {
                EmitControlDiagnostic(
                    "PXWASM-DEBUG-002",
                    "Story step requires a breakpoint-paused Runtime. Continue from a manual pause or set a breakpoint first.");
                return 0;
            }
            m_session->VM().DebugStep();
            m_paused = true;
            SetPerformanceAudioPaused(true);
        } else if (command == "advance") {
            m_operationFocusOverride.reset();
            const auto previewResult = m_previewSession->Advance();
            if (!previewResult.accepted) {
                EmitPreviewDiagnostics(previewResult, "PXWASM-ADVANCE-001");
                return 0;
            }
        } else if (command == "input") {
            m_operationFocusOverride.reset();
            if (payload.value("action", std::string{}) != "pointerClick" ||
                !payload.contains("x") || !payload["x"].is_number() ||
                !payload.contains("y") || !payload["y"].is_number()) {
                EmitControlDiagnostic(
                    "PXWASM-INPUT-001",
                    "Preview input requires a pointerClick action with finite logical coordinates.");
                return 0;
            }
            const float x = payload["x"].get<float>();
            const float y = payload["y"].get<float>();
            int width = 0;
            int height = 0;
            m_runtime.Renderer().GetLogicalSize(width, height);
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0f ||
                y < 0.0f || x > static_cast<float>(width) ||
                y > static_cast<float>(height)) {
                EmitControlDiagnostic(
                    "PXWASM-INPUT-002",
                    "Preview pointer coordinates are outside the Runtime logical surface.");
                return 0;
            }
            m_pendingPointerClick = px::Vec2{x, y};
        } else if (command == "selectChoice") {
            m_operationFocusOverride.reset();
            const int index = payload.value("index", -1);
            if (index < 0 ||
                index >= static_cast<int>(m_session->VM().Choices().size()))
                return 0;
            const auto previewResult = m_previewSession->SelectChoice(index);
            if (!previewResult.accepted) {
                EmitPreviewDiagnostics(previewResult, "PXWASM-CHOICE-001");
                return 0;
            }
        } else if (command == "setAudioLevels") {
            const auto levels = payload.find("levels");
            if (levels == payload.end() || !levels->is_object() ||
                levels->empty()) {
                EmitControlDiagnostic(
                    "PXWASM-AUDIO-001",
                    "setAudioLevels requires at least one named audio bus level.");
                return 0;
            }
            const auto applyLevel = [&](const char* name,
                                        const auto& setter) -> bool {
                const auto value = levels->find(name);
                if (value == levels->end()) return true;
                if (!value->is_number_integer()) return false;
                const int volume = value->get<int>();
                if (volume < 0 || volume > 128) return false;
                setter(volume);
                return true;
            };
            if (!applyLevel("main", [this](const int value) {
                    m_runtime.Audio().SetMainVolume(value);
                }) ||
                !applyLevel("music", [this](const int value) {
                    m_runtime.Audio().SetBGMVolume(value);
                }) ||
                !applyLevel("voice", [this](const int value) {
                    m_runtime.Audio().SetVoiceVolume(value);
                }) ||
                !applyLevel("sfx", [this](const int value) {
                    m_runtime.Audio().SetSEVolume(value);
                }) ||
                !applyLevel("ambience", [this](const int value) {
                    m_runtime.Audio().SetAmbienceVolume(value);
                })) {
                EmitControlDiagnostic(
                    "PXWASM-AUDIO-002",
                    "Audio bus levels must be integers from 0 through 128.");
                return 0;
            }
            const auto audio = m_runtime.Audio().CaptureState();
            m_protocol.Emit(
                "audioState",
                Json{{"status", audio.mainVolume == 0 ? "muted" : "running"},
                     {"main", audio.mainVolume},
                     {"music", audio.musicVolume},
                     {"voice", audio.voiceVolume},
                     {"sfx", audio.sfxVolume},
                     {"ambience", audio.ambienceVolume}}
                    .dump());
        } else if ((command == "seekStory" || command == "seek") &&
                   (payload.contains("operation") ||
                    payload.contains("blockId"))) {
            int operationIndex = -1;
            const auto operation = payload.find("operation");
            const auto blockId = payload.find("blockId");
            if (operation != payload.end()) {
                if (!operation->is_number_integer()) {
                    EmitControlDiagnostic(
                        "PXWASM-OP-SEEK-001",
                        "Story operation seek requires an integer operation index.");
                    return 0;
                }
                operationIndex = operation->get<int>();
            } else if (blockId != payload.end() && blockId->is_string()) {
                const std::string requestedSourceId = blockId->get<std::string>();
                const auto runtimeIr = px::sdk::ParseRuntimeIr(m_runtimeIrJson);
                const auto found = std::find_if(
                    runtimeIr.document.operations.begin(),
                    runtimeIr.document.operations.end(),
                    [&requestedSourceId](const px::sdk::RuntimeIrOperation& item) {
                        return item.sourceId == requestedSourceId;
                    });
                if (found != runtimeIr.document.operations.end()) {
                    operationIndex = static_cast<int>(
                        std::distance(runtimeIr.document.operations.begin(),
                                      found));
                }
            }
            if (m_runtimeIrJson.empty() || operationIndex < 0) {
                EmitControlDiagnostic(
                    "PXWASM-OP-SEEK-001",
                    "The requested Story Block does not map to an operation in the applied Runtime IR.");
                return 0;
            }
            std::vector<int> branchPath;
            if (const auto encodedPath = payload.find("branchPath");
                encodedPath != payload.end()) {
                if (!encodedPath->is_array()) {
                    EmitControlDiagnostic(
                        "PXWASM-OP-SEEK-004",
                        "Story seek branchPath must be an array of choice indices.");
                    return 0;
                }
                for (const auto& choice : *encodedPath) {
                    if (!choice.is_number_integer() || choice.get<int>() < 0) {
                        EmitControlDiagnostic(
                            "PXWASM-OP-SEEK-004",
                            "Story seek branchPath must contain non-negative choice indices.");
                        return 0;
                    }
                    branchPath.push_back(choice.get<int>());
                }
            }
            const auto previewResult = m_previewSession->SeekStory(
                {operationIndex, std::move(branchPath)});
            if (!previewResult.accepted) {
                EmitPreviewDiagnostics(previewResult, "PXWASM-OP-SEEK-002");
                return 0;
            }
            m_operationFocusOverride = operationIndex;
            m_choices.clear();
            m_session->Stage().Update(3600.0f);
            if (m_session->VM().State() == px::vn::VMState::WaitingClick)
                m_session->Dialogue().ShowAll();
            if (m_session->VM().State() == px::vn::VMState::WaitingChoice) {
                for (const auto& choice : m_session->VM().Choices())
                    m_choices.push_back(choice.text);
            }
            m_paused = true;
            SetPerformanceAudioPaused(true);
            if (!m_uiPreviewActive)
                (void)m_hud.ShowHUD(CurrentDialogueView());
            m_lastFocus.clear();
            m_lastVmState.clear();
            m_protocol.Emit(
                "state",
                Json{{"status", "paused"},
                     {"mode", "wasm"},
                     {"reason", "operationSeek"},
                     {"operation", operationIndex + 1},
                     {"operationIndex", operationIndex},
                     {"operationTotal",
                      px::sdk::ParseRuntimeIr(m_runtimeIrJson)
                          .document.operations.size()}}
                    .dump());
            EmitFocus(true);
        } else if (command == "seekTimeline" &&
                   payload.contains("playbackHandle")) {
            const auto handle = payload.find("playbackHandle");
            const double time = payload.value(
                "time", std::numeric_limits<double>::quiet_NaN());
            if (handle == payload.end() || !handle->is_number_unsigned()) {
                EmitControlDiagnostic(
                    "PXWASM-TIMELINE-SEEK-001",
                    "Timeline seek requires an unsigned playbackHandle.");
                return 0;
            }
            const auto previewResult = m_previewSession->SeekTimeline(
                {handle->get<std::uint64_t>(), time});
            if (!previewResult.accepted) {
                EmitPreviewDiagnostics(previewResult,
                                       "PXWASM-TIMELINE-SEEK-002");
                return 0;
            }
            m_paused = true;
            SetPerformanceAudioPaused(true);
        } else if (command == "seek" || command == "seekTimeline") {
            m_operationFocusOverride.reset();
            const double time = payload.value(
                "time", std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(time) || time < 0.0 ||
                !m_performanceSequence) {
                m_protocol.Emit(
                    "diagnostics",
                    Json{{"diagnostics",
                          Json::array({{{"severity", "error"},
                                        {"code", "PXWASM-PERFORMANCE-010"},
                                        {"category", "Preview.Performance"},
                                        {"message", !m_performanceSequence
                                                        ? "No Performance document is applied to this Preview session."
                                                        : "Performance seek time is invalid."},
                                        {"suggestion", "Apply the saved Stage Performance, then seek again."},
                                        {"source", {{"documentId", request.documentId}}}}})}}
                        .dump());
                return 0;
            }
            const auto plan = m_performanceSequence->Sample(time);
            px::preview::ApplyPerformancePreviewPlan(*m_session, plan);
            if (!ApplyPerformanceUi(plan)) return 0;
            m_performanceAudioSignature = plan.audio.signature;
            m_paused = true;
            SetPerformanceAudioPaused(true);
            m_performanceTime = plan.seekTime;
            m_lastPerformanceEventTime = plan.seekTime;
            m_performanceRevision = plan.revision;
            m_protocol.Emit(
                "state",
                Json{{"status", "paused"},
                     {"mode", "wasm"},
                     {"performanceTime", plan.seekTime},
                     {"performanceRevision", plan.revision},
                     {"performanceNodeCount", plan.nodes.size()}}
                    .dump());
        } else if (command == "activateUiControl") {
            const std::string nodeId = payload.value("nodeId", std::string{});
            if (!m_uiPreviewActive || nodeId.empty()) return 0;
            m_lastUiAction.clear();
            const auto status = m_hud.ActivateUiControl(nodeId);
            if (!status) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic : status.Diagnostics()) {
                    diagnostics.push_back(
                        {{"severity", "error"},
                         {"code", diagnostic.code},
                         {"category", diagnostic.category},
                         {"message", diagnostic.message},
                         {"details", diagnostic.details},
                         {"source", {{"documentId", request.documentId},
                                     {"blockId", nodeId}}}});
                }
                m_protocol.Emit("diagnostics",
                                Json{{"diagnostics", diagnostics}}.dump());
                return 0;
            }
            m_protocol.Emit(
                "debug",
                Json{{"kind", "uiControlActivated"},
                     {"nodeId", nodeId},
                     {"actionId", m_lastUiAction}}
                    .dump());
        } else if (command == "simulateProgression") {
            if (!payload.contains("progression") ||
                !payload["progression"].is_object() ||
                !payload.contains("state") || !payload["state"].is_object()) {
                EmitControlDiagnostic(
                    "PXWASM-PROGRESSION-CONTROL-001",
                    "simulateProgression requires a canonical Progression document and profile state.");
                return 0;
            }
            const auto simulated = px::preview::SimulateProgressionPreview(
                Json{{"document", payload["progression"]},
                     {"state", payload["state"]}}
                    .dump());
            if (!simulated) {
                Json diagnostics = Json::array();
                for (const auto& diagnostic : simulated.Diagnostics()) {
                    diagnostics.push_back(
                        {{"severity", "error"},
                         {"code", diagnostic.code},
                         {"category", diagnostic.category},
                         {"message", diagnostic.message},
                         {"details", diagnostic.details},
                         {"suggestion",
                          "Fix the identified Progression draft field and simulate again."},
                         {"source",
                          {{"documentId", diagnostic.source.resourceId},
                           {"blockId", diagnostic.source.nodeId},
                           {"path", diagnostic.source.path},
                           {"property", diagnostic.source.property},
                           {"line", diagnostic.source.line}}}});
                }
                m_protocol.Emit(
                    "diagnostics",
                    Json{{"diagnostics", std::move(diagnostics)}}.dump());
                return 0;
            }
            Json nodes = Json::array();
            for (const auto& node : simulated.Value().nodes) {
                nodes.push_back(
                    {{"nodeId", node.nodeId},
                     {"unlocked", node.unlocked},
                     {"unmetRequirementIds", node.unmetRequirementIds}});
            }
            m_protocol.Emit(
                "debug",
                Json{{"kind", "progressionSimulation"},
                     {"result",
                      {{"revision", simulated.Value().revision},
                       {"nodes", std::move(nodes)}}}}
                    .dump());
            return 1;
        } else if (command == "setBreakpoints") {
            const auto lines = payload.find("lines");
            if (lines == payload.end() || !lines->is_array()) {
                EmitControlDiagnostic(
                    "PXWASM-DEBUG-003",
                    "setBreakpoints requires an array of positive source lines.");
                return 0;
            }
            std::set<int> requestedLines;
            for (const auto& line : *lines) {
                if (!line.is_number_integer() || line.get<int>() <= 0) {
                    EmitControlDiagnostic(
                        "PXWASM-DEBUG-003",
                        "Story breakpoint lines must be positive integers.");
                    return 0;
                }
                requestedLines.insert(line.get<int>());
            }
            std::set<int> availableLines;
            for (const auto& operation : m_session->VM().CurrentProgram().code)
                if (operation.line > 0) availableLines.insert(operation.line);
            m_session->VM().ClearBreakpoints();
            Json verified = Json::array();
            Json unresolved = Json::array();
            for (const int line : requestedLines) {
                if (availableLines.contains(line)) {
                    m_session->VM().ToggleBreakpoint(line);
                    verified.push_back(line);
                } else {
                    unresolved.push_back(line);
                }
            }
            EmitDebugControlResult(command, false, std::move(verified),
                                   std::move(unresolved));
            return 1;
        } else if (command == "setScriptBreakpoints") {
            const auto breakpoints = payload.find("breakpoints");
            if (!m_scriptHost || breakpoints == payload.end() ||
                !breakpoints->is_array()) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-DEBUG-002",
                    "setScriptBreakpoints requires source and line objects after a script extension graph is applied.");
                return 0;
            }
            std::vector<px::script::DebugBreakpoint> acceptedBreakpoints;
            Json verified = Json::array();
            Json unresolved = Json::array();
            for (const auto& breakpoint : *breakpoints) {
                if (!breakpoint.is_object() ||
                    !breakpoint.contains("source") ||
                    !breakpoint["source"].is_string() ||
                    !breakpoint.contains("line") ||
                    !breakpoint["line"].is_number_integer()) {
                    EmitControlDiagnostic(
                        "PXWASM-SCRIPT-DEBUG-003",
                        "Script breakpoints require a project-relative source and positive line.");
                    return 0;
                }
                std::string source = breakpoint["source"].get<std::string>();
                constexpr std::string_view projectPrefix = "/project/";
                if (source.starts_with(projectPrefix))
                    source.erase(0, projectPrefix.size());
                const int line = breakpoint["line"].get<int>();
                const bool safe = line > 0 && !source.empty() &&
                                  !source.starts_with('/') &&
                                  source.find("..") == std::string::npos &&
                                  source.starts_with("Content/Extensions/") &&
                                  source.ends_with(".js");
                const auto contents = safe
                                          ? m_runtime.VFS().ReadText(source)
                                          : std::nullopt;
                const Json item{{"source", source}, {"line", line}};
                const std::size_t lineCount =
                    contents ? 1 + static_cast<std::size_t>(
                                       std::ranges::count(*contents, '\n'))
                             : 0;
                if (contents && static_cast<std::size_t>(line) <= lineCount) {
                    acceptedBreakpoints.push_back({source, line});
                    verified.push_back(item);
                } else {
                    unresolved.push_back(item);
                }
            }
            m_scriptBreakpoints = m_scriptHost->SetDebugBreakpoints(
                std::move(acceptedBreakpoints));
            EmitDebugControlResult(
                command, false, nullptr, nullptr,
                Json{{"scriptBreakpoints", std::move(verified)},
                     {"unresolvedScriptBreakpoints", std::move(unresolved)}});
            return 1;
        } else if (command == "scriptPause") {
            if (!m_scriptHost || !m_scriptHost->DebugPause()) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-DEBUG-004",
                    "Script execution is already paused or no script debug session is available.");
                return 0;
            }
            EmitDebugControlResult(command, false);
            return 1;
        } else if (command == "scriptContinue") {
            if (!m_scriptHost || !m_scriptHost->DebugContinue()) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-DEBUG-005",
                    "Script continue requires paused execution.");
                return 0;
            }
            EmitDebugControlResult(command, false);
            return 1;
        } else if (command == "scriptStep") {
            if (!m_scriptHost || !m_scriptHost->DebugStep()) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-DEBUG-006",
                    "Script step requires paused execution.");
                return 0;
            }
            EmitDebugControlResult(command, false);
            return 1;
        } else if (command == "evaluateScriptWatches") {
            const auto watches = payload.find("watches");
            if (!m_scriptHost || !m_scriptHost->CaptureDebugState().paused ||
                watches == payload.end() || !watches->is_array()) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-DEBUG-007",
                    "Script watch evaluation requires paused execution and an array of expressions.");
                return 0;
            }
            Json results = Json::array();
            for (const auto& expression : *watches) {
                if (!expression.is_string() ||
                    expression.get_ref<const std::string&>().size() > 256) {
                    EmitControlDiagnostic(
                        "PXWASM-SCRIPT-DEBUG-008",
                        "Script watch expressions must be strings up to 256 characters.");
                    return 0;
                }
                const std::string value = expression.get<std::string>();
                if (const auto result = m_scriptHost->EvaluateDebugWatch(value)) {
                    results.push_back({{"expression", result->name},
                                       {"value", result->value},
                                       {"available", true}});
                } else {
                    results.push_back({{"expression", value},
                                       {"value", "<unavailable>"},
                                       {"available", false}});
                }
            }
            EmitDebugControlResult(
                command, false, nullptr, nullptr,
                Json{{"scriptWatches", std::move(results)}});
            return 1;
        } else if (command == "capture") {
            const auto checkpoint = m_previewSession->CaptureCheckpoint();
            if (!checkpoint.accepted || !checkpoint.checkpoint) {
                EmitPreviewDiagnostics(checkpoint, "PXWASM-CAPTURE-001");
                return 0;
            }
            EmitFocus(true);
            EmitDebugControlResult(
                command, true, nullptr, nullptr,
                Json{{"checkpointId", checkpoint.checkpoint->id},
                     {"operationIndex",
                      checkpoint.checkpoint->operationIndex},
                     {"branchPath", checkpoint.checkpoint->branchPath}});
            (void)m_previewSession->Events();
            return 1;
        } else if (command == "restoreCheckpoint") {
            const auto checkpointId = payload.find("checkpointId");
            if (checkpointId == payload.end() ||
                !checkpointId->is_number_unsigned()) {
                EmitControlDiagnostic(
                    "PXWASM-CHECKPOINT-001",
                    "restoreCheckpoint requires an unsigned checkpointId.");
                return 0;
            }
            const auto restored = m_previewSession->RestoreCheckpoint(
                checkpointId->get<std::uint64_t>());
            if (!restored.accepted) {
                EmitPreviewDiagnostics(restored,
                                       "PXWASM-CHECKPOINT-002");
                return 0;
            }
            m_paused = true;
            SetPerformanceAudioPaused(true);
            EmitDebugControlResult(
                command, true, nullptr, nullptr,
                Json{{"checkpointId", checkpointId->get<std::uint64_t>()}});
            (void)m_previewSession->Events();
            return 1;
        } else if (command == "stop") {
            StopPerformanceAudio();
            m_stopped = true;
            m_previewSaves.clear();
            m_protocol.Emit("state",
                            Json{{"status", "stopped"}, {"mode", "wasm"}}.dump());
        } else {
            return 0;
        }
        if (command != "activateUiControl" && command != "seek" &&
            command != "seekStory" && command != "seekTimeline")
            EmitDebugControlResult(command, false);
        (void)m_previewSession->Events();
        return 1;
    }

    int Resize(const int width, const int height, const double dpr,
               const bool visible) {
        if (m_visible != visible && !m_paused)
            SetPerformanceAudioPaused(!visible);
        m_visible = visible;
        if (!m_ready || width <= 0 || height <= 0 || dpr <= 0.0) return 0;
        const auto resized = m_previewSession->Resize(
            width, height, static_cast<float>(dpr));
        if (!resized.accepted) {
            EmitPreviewDiagnostics(resized, "PXWASM-RESIZE-001");
            return 0;
        }
        m_protocol.Emit("state",
                        Json{{"status", "resized"},
                             {"cssWidth", width},
                             {"cssHeight", height},
                             {"devicePixelRatio", dpr},
                             {"visible", visible}}
                            .dump());
        (void)m_previewSession->Events();
        return 1;
    }

    int Tick(const double) {
        if (!m_ready || m_stopped || !m_visible || !m_session) return 0;
        if (!m_runtime.BeginFrame()) return 0;
        if (m_releaseInjectedPointer) {
            m_runtime.GetInput().InjectFrame(m_lastInjectedPointer.x,
                                             m_lastInjectedPointer.y, false);
            m_releaseInjectedPointer = false;
        }
        if (m_pendingPointerClick) {
            m_lastInjectedPointer = *m_pendingPointerClick;
            m_runtime.GetInput().InjectFrame(m_lastInjectedPointer.x,
                                             m_lastInjectedPointer.y, true);
            m_pendingPointerClick.reset();
            m_releaseInjectedPointer = true;
        }
        const float delta = m_runtime.GetClock().DeltaSeconds();
        if (m_scriptHost) {
            m_scriptHost->Update(delta);
            m_hud.Actions().Update(delta);
            if (m_session->VM().State() ==
                    px::vn::VMState::WaitingExternal &&
                !m_scriptHost->HasPendingCommand() && !m_scriptHost->HasPendingAction())
                m_session->VM().NotifyExternalDone();
        }
        const std::uint64_t nowMs = m_runtime.GetClock().NowMs();
        if (!m_paused) {
            const auto ticked = m_previewSession->Tick(nowMs, delta);
            if (!ticked.accepted) {
                EmitPreviewDiagnostics(ticked, "PXWASM-TICK-001");
                m_paused = true;
                return 0;
            }
        }
        if (!m_paused && m_performanceSequence) {
            const double previousPerformanceTime = m_performanceTime;
            m_performanceTime = std::min(
                m_performanceSequence->duration,
                m_performanceTime + static_cast<double>(delta));
            const auto performancePlan =
                m_performanceSequence->Sample(m_performanceTime);
            const bool audioBoundaryChanged =
                performancePlan.audio.signature !=
                m_performanceAudioSignature;
            px::preview::ApplyPerformancePreviewPlan(
                *m_session, performancePlan, false, audioBoundaryChanged);
            if (!ApplyPerformanceUi(performancePlan)) {
                m_paused = true;
                return 0;
            }
            m_performanceAudioSignature = performancePlan.audio.signature;
            DispatchPerformanceEvents(previousPerformanceTime,
                                      m_performanceTime);
            if (m_performanceTime - m_lastPerformanceEventTime >=
                (1.0 / 15.0)) {
                m_lastPerformanceEventTime = m_performanceTime;
                m_protocol.Emit(
                    "state",
                    Json{{"status", "running"},
                         {"mode", "wasm"},
                         {"performanceTime", m_performanceTime},
                         {"performanceRevision", m_performanceRevision}}
                        .dump());
            }
            if (m_performanceTime >= m_performanceSequence->duration) {
                m_paused = true;
                m_protocol.Emit(
                    "state",
                    Json{{"status", "paused"},
                         {"mode", "wasm"},
                         {"reason", "performanceEnded"},
                         {"performanceTime", m_performanceTime},
                         {"performanceRevision", m_performanceRevision}}
                        .dump());
            }
        }
        UpdatePlaybackModes(nowMs);
        EmitScriptDebugStateIfChanged();
        m_choices.clear();
        if (m_session->VM().State() == px::vn::VMState::WaitingChoice) {
            for (const auto& choice : m_session->VM().Choices())
                m_choices.push_back(choice.text);
        }
        if (!m_uiPreviewActive &&
            m_hud.CurrentScreen() == px::ui::GalgameUI::Screen::HUD)
            (void)m_hud.RefreshHUD(CurrentDialogueView());
        int width = 0;
        int height = 0;
        m_runtime.Renderer().GetLogicalSize(width, height);
        const bool consumed = m_hud.Update(m_runtime.GetInput(), width, height,
                                           delta);
        if (!consumed && m_runtime.GetInput().LeftClick() &&
            m_hud.CurrentScreen() == px::ui::GalgameUI::Screen::HUD &&
            m_session->VM().State() != px::vn::VMState::WaitingChoice) {
            (void)m_previewSession->Advance();
            m_autoMode = false;
            m_skipMode = false;
            m_autoTimerStartedAtMs = 0;
        }
        m_session->Stage().Render();
        m_hud.Render(m_runtime.Renderer());
        m_runtime.EndFrame();
        EmitFocus(false);
        (void)m_previewSession->Events();
        return 1;
    }

    const char* DrainEvents() {
        m_drained = m_protocol.DrainEvents();
        return m_drained.c_str();
    }

private:
    static constexpr std::size_t kRuntimeSnapshotItemLimit = 128;

    struct ScriptCheckpoint {
        px::script::PendingCommandsState commands;
        px::script::PendingActionsState actions;
    };

    std::optional<px::sdk::PreviewSafety> InspectOperationSafety(
        const px::vn::Command& command) {
        if (command.type != "action") return std::nullopt;
        const Json payload = Json::parse(command.Get("value"), nullptr, false);
        if (payload.is_discarded() || !payload.is_object() ||
            !payload.contains("id") || !payload["id"].is_string())
            return px::sdk::PreviewSafety{};
        const auto* descriptor = m_hud.Actions().Catalog().Find(
            payload["id"].get<std::string>());
        if (!descriptor) return px::sdk::PreviewSafety{};
        return px::sdk::PreviewSafety{
            descriptor->previewSafe, descriptor->deterministic,
            descriptor->seekSafe, descriptor->rollbackSafe};
    }

    std::shared_ptr<const void> CaptureExternalCheckpoint() const {
        if (!m_scriptHost) return {};
        return std::make_shared<ScriptCheckpoint>(ScriptCheckpoint{
            m_scriptHost->CapturePending(),
            m_scriptHost->CapturePendingActions()});
    }

    px::Status RestoreExternalCheckpoint(
        const std::shared_ptr<const void>& opaque) {
        if (!opaque) {
            if (m_scriptHost) m_scriptHost->CancelPending();
            return px::Status::Ok();
        }
        if (!m_scriptHost) {
            px::diag::Diagnostic diagnostic;
            diagnostic.severity = px::diag::Severity::Error;
            diagnostic.code = "PXWASM-CHECKPOINT-SCRIPT-001";
            diagnostic.category = "Preview.Script";
            diagnostic.message =
                "Script checkpoint cannot be restored without the JavaScript host.";
            return px::Status::Fail(std::move(diagnostic));
        }
        const auto checkpoint =
            std::static_pointer_cast<const ScriptCheckpoint>(opaque);
        m_scriptHost->CancelPending();
        if (px::Status status =
                m_scriptHost->RestorePending(checkpoint->commands);
            !status)
            return status;
        return m_scriptHost->RestorePendingActions(checkpoint->actions);
    }

    void EmitPreviewDiagnostics(
        const px::sdk::PreviewCommandResult& result,
        const std::string_view fallbackCode) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : result.diagnostics) {
            Json entry = PreviewDiagnosticJson(
                diagnostic, m_protocol.DocumentId());
            if (diagnostic.code.empty()) entry["code"] = fallbackCode;
            diagnostics.push_back(std::move(entry));
        }
        if (diagnostics.empty())
            diagnostics.push_back(
                {{"severity", "error"},
                 {"code", fallbackCode},
                 {"category", "Preview.Session"},
                 {"message", "Preview session operation was rejected."},
                 {"details", PreviewStatusName(result.status)}});
        m_protocol.Emit("diagnostics",
                        Json{{"diagnostics", std::move(diagnostics)}}.dump());
        (void)m_previewSession->Events();
    }

    px::Status EnsurePreviewRoute(const std::string_view route) {
        if (route.empty())
            return UiFailure("PXWASM-UI-ROUTE-001",
                             "Runtime route id cannot be empty.");
        const std::string id(route);
        if (m_registeredRoutes.contains(id)) return px::Status::Ok();
        const auto registered = m_session->Routes().Register(
            id, [id]() -> px::Result<std::unique_ptr<px::ui::Control>> {
                return px::Result<std::unique_ptr<px::ui::Control>>::Success(
                    std::make_unique<px::ui::Control>("PreviewRoute:" + id));
            });
        if (registered) m_registeredRoutes.insert(id);
        return registered;
    }

    px::Status PresentPreviewScreen(const std::string_view requestedRoute) {
        const std::string route = requestedRoute == "saveload"
                                      ? "load"
                                      : std::string(requestedRoute);
        if (route == "save")
            return m_hud.ShowSaveLoad(true, PreviewSaveItems(true));
        if (route == "load")
            return m_hud.ShowSaveLoad(false, PreviewSaveItems(false));
        if (route == "settings")
            return m_hud.ShowSettings(m_previewSettings);
        if (route == "backlog") {
            auto items = PreviewBacklogItems();
            if (items.empty())
                return UiFailure(
                    "PXWASM-UI-BACKLOG-001",
                    "The Preview backlog is empty.", {},
                    "Advance dialogue before opening the backlog.");
            return m_hud.ShowBacklog(std::move(items));
        }
        if (route == "gallery") {
            auto items = PreviewGalleryItems();
            if (items.empty())
                return UiFailure(
                    "PXWASM-UI-GALLERY-001",
                    "No gallery entries are present in the active Preview dependency graph.",
                    {},
                    "Add a referenced gallery resource, then restart Preview.");
            return m_hud.ShowGallery(std::move(items));
        }
        if (route == "title") return m_hud.ShowTitle();
        if (route == "hud" || route == "game") {
            m_uiPreviewActive = false;
            return m_hud.ShowHUD(CurrentDialogueView());
        }
        return UiFailure(
            "PXWASM-UI-ROUTE-002",
            "The active WASM Preview bundle has no presentation for Runtime route: " +
                route,
            "route=" + route,
            "Add the route UI scene to the active Preview dependency graph.");
    }

    px::Status OpenPreviewOverlay(const std::string_view route) {
        if (const auto ensured = EnsurePreviewRoute(route); !ensured)
            return ensured;
        if (const auto routed = m_session->Routes().ShowModal(route); !routed)
            return routed;
        return PresentPreviewScreen(route);
    }

    void PresentPreviewRoute(const std::string_view route,
                             const std::string_view operation) {
        std::string target(route);
        if (operation == "back") {
            const auto& router = m_session->Routes();
            target = router.CurrentModalRoute().empty()
                         ? std::string(router.CurrentRoute())
                         : std::string(router.CurrentModalRoute());
            if (target.empty()) target = m_uiPreviewActive ? "title" : "hud";
        }
        const auto status = PresentPreviewScreen(target);
        if (!status)
            EmitStatusDiagnostics(status, "PXWASM-UI-ROUTE-003");
    }

    void UpdatePlaybackModes(const std::uint64_t nowMs) {
        if (!m_session || m_paused || m_uiPreviewActive ||
            m_hud.CurrentScreen() != px::ui::GalgameUI::Screen::HUD) {
            m_autoTimerStartedAtMs = 0;
            return;
        }
        const auto state = m_session->VM().State();
        if (state == px::vn::VMState::WaitingChoice) {
            m_skipMode = false;
            m_autoTimerStartedAtMs = 0;
            return;
        }
        if (m_skipMode && m_previewSettings.skipReadOnly &&
            !m_session->VM().CurrentLineSeen()) {
            m_skipMode = false;
        }
        if (m_skipMode &&
            (state == px::vn::VMState::WaitingClick ||
             state == px::vn::VMState::WaitingTimer)) {
            if (!m_session->Dialogue().Finished())
                m_session->Dialogue().ShowAll();
            (void)m_previewSession->Advance();
            m_autoTimerStartedAtMs = 0;
            return;
        }
        if (m_autoMode && state == px::vn::VMState::WaitingClick &&
            m_session->Dialogue().Finished()) {
            constexpr std::uint64_t kPreviewAutoWaitMs = 1000;
            if (m_autoTimerStartedAtMs == 0)
                m_autoTimerStartedAtMs = nowMs;
            else if (nowMs - m_autoTimerStartedAtMs >= kPreviewAutoWaitMs) {
                (void)m_previewSession->Advance();
                m_autoTimerStartedAtMs = 0;
            }
            return;
        }
        m_autoTimerStartedAtMs = 0;
    }

    static std::optional<int> ParseInteger(const std::string& text) {
        if (text.empty()) return std::nullopt;
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' ||
            value < std::numeric_limits<int>::min() ||
            value > std::numeric_limits<int>::max())
            return std::nullopt;
        return static_cast<int>(value);
    }

    static std::optional<bool> ParseBoolean(const std::string& text) {
        if (text == "true") return true;
        if (text == "false") return false;
        return std::nullopt;
    }

    static std::optional<std::string> InvocationArgument(
        const px::ui::ActionInvocation& invocation,
        const std::string_view name) {
        const auto found = invocation.arguments.find(std::string(name));
        if (found == invocation.arguments.end()) return std::nullopt;
        const auto& value = found->second;
        if (const auto* text = value.TryGet<std::string>()) return *text;
        if (const auto* integer = value.TryGet<std::int64_t>())
            return std::to_string(*integer);
        if (const auto* number = value.TryGet<double>())
            return std::to_string(*number);
        if (const auto* boolean = value.TryGet<bool>())
            return *boolean ? "true" : "false";
        if (const auto* resource = value.TryGet<px::ResourceRefValue>())
            return resource->lastKnownPath;
        return std::nullopt;
    }

    px::Status UiFailure(std::string code, std::string message,
                         std::string details = {},
                         std::string quickFix = {}) const {
        px::diag::Diagnostic diagnostic;
        diagnostic.severity = px::diag::Severity::Error;
        diagnostic.code = std::move(code);
        diagnostic.category = "Preview.UI";
        diagnostic.message = std::move(message);
        diagnostic.details = std::move(details);
        diagnostic.quickFix = std::move(quickFix);
        diagnostic.source.resourceId = m_protocol.DocumentId();
        return px::Status::Fail(std::move(diagnostic));
    }

    px::Status UnsupportedUiCapability(
        const std::string_view action, const std::string_view capability,
        const std::string_view message, const bool nativeCheckAvailable,
        const std::string_view suggestion) {
        m_protocol.Emit(
            "unsupported",
            Json{{"code", "PXWASM-UI-CAPABILITY-001"},
                 {"category", "Preview.UI"},
                 {"message", message},
                 {"actionId", action},
                 {"capability", capability},
                 {"nativeCheckAvailable", nativeCheckAvailable},
                 {"suggestion", suggestion},
                 {"source", {{"documentId", m_protocol.DocumentId()}}}}
                .dump());
        return UiFailure(
            "PXWASM-UI-CAPABILITY-001", std::string(message),
            "action=" + std::string(action) + "; capability=" +
                std::string(capability) + "; nativeCheckAvailable=" +
                (nativeCheckAvailable ? "true" : "false"),
            std::string(suggestion));
    }

    px::ui::DialoguePresentation CurrentDialogueView() const {
        auto view = DialogueView(*m_session, m_choices);
        view.autoMode = m_autoMode;
        view.skipMode = m_skipMode;
        view.textScale = m_previewSettings.textScale;
        view.reducedMotion = m_previewSettings.reducedMotion;
        return view;
    }

    std::vector<px::ui::GalgameItem> PreviewSaveItems(
        const bool saveMode) const {
        std::vector<px::ui::GalgameItem> items;
        for (int slot = 0; slot < 6; ++slot) {
            const auto saved = m_previewSaves.find(slot);
            const bool exists = saved != m_previewSaves.end();
            std::string label = "空白存檔";
            std::string subtitle = "只保留在本次 WASM Preview 工作階段";
            if (exists) {
                label = saved->second.vm.chapter.empty()
                            ? "未命名章節"
                            : saved->second.vm.chapter;
                subtitle = "記憶體存檔 · 不會寫入正式遊戲存檔";
            }
            items.push_back(
                {"preview-slot-" + std::to_string(slot),
                 "#" + std::to_string(slot + 1) + "  " + label,
                 std::move(subtitle), "", !saveMode && !exists,
                 saveMode ? "save.slot" : "load.slot",
                 std::to_string(slot)});
        }
        return items;
    }

    std::vector<px::ui::GalgameItem> PreviewBacklogItems() const {
        std::vector<px::ui::GalgameItem> items;
        const auto& entries = m_session->Backlog().Entries();
        items.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            const std::string speaker =
                entry.isChoice ? "▶ 選擇" : entry.speaker;
            const bool hasVoice = !entry.voice.empty();
            items.push_back(
                {"preview-backlog-" + std::to_string(index),
                 speaker.empty() ? entry.text
                                 : speaker + "　" + entry.text,
                 hasVoice ? "♪ 點擊重播語音"
                          : "此行沒有可重播的語音",
                 "", !hasVoice, "backlog.voice",
                 std::to_string(index)});
        }
        return items;
    }

    std::vector<px::ui::GalgameItem> PreviewGalleryItems() const {
        std::vector<px::ui::GalgameItem> items;
        items.reserve(m_catalog.Gallery().size());
        for (const auto& entry : m_catalog.Gallery()) {
            items.push_back(
                {entry.id, entry.title, "WASM Preview 資源", entry.thumbnail,
                 false, "cg.view", entry.image});
        }
        return items;
    }

    void EmitUiActionApplied(
        const std::string_view action, const std::string_view argument,
        const px::ui::ActionContext* context = nullptr) {
        Json event{{"scope", "ui"},
                   {"stream", "runtime"},
                   {"level", "info"},
                   {"message", "UI action applied: " + std::string(action)},
                   {"actionId", action},
                   {"argument", argument}};
        if (context) {
            event["nodeId"] = context->sourceNode.ToString();
            event["signal"] = context->signal;
        }
        m_protocol.Emit("output", event.dump());
    }

    px::Status ExecuteUiAction(
        const px::ui::ActionInvocation& invocation) {
        std::string_view argumentName;
        if (invocation.action == "choice.select" ||
            invocation.action == "backlog.voice" ||
            invocation.action == "backlog.rollback")
            argumentName = "index";
        else if (invocation.action == "load.slot" ||
                 invocation.action == "save.slot")
            argumentName = "slot";
        else if (invocation.action == "cg.view")
            argumentName = "resource";
        else if (invocation.action.ends_with(".value"))
            argumentName = "value";

        std::string argument;
        if (!argumentName.empty()) {
            const auto converted = InvocationArgument(invocation, argumentName);
            if (!converted)
                return UiFailure(
                    "PXWASM-UI-ACTION-002",
                    "WASM Preview could not read the typed UI action argument.",
                    invocation.action + "." + std::string(argumentName),
                    "Fix the Action argument in UI Designer and try again.");
            argument = *converted;
        }
        return ExecuteUiAction(invocation.action, argument,
                               &invocation.context);
    }

    px::Status ExecuteUiAction(
        const std::string_view action, const std::string& argument,
        const px::ui::ActionContext* context = nullptr) {
        if (!m_session)
            return UiFailure("PXWASM-UI-ACTION-003",
                             "WASM Preview has no active Runtime session.");
        const auto complete = [&](px::Status status) {
            if (status) EmitUiActionApplied(action, argument, context);
            return status;
        };

        if (action == "advance") {
            const auto advanced = m_previewSession->Advance();
            if (!advanced.accepted)
                return UiFailure(
                    "PXWASM-UI-ACTION-ADVANCE-001",
                    advanced.diagnostics.empty()
                        ? "Preview could not advance."
                        : advanced.diagnostics.front().message);
            return complete(px::Status::Ok());
        }
        if (action == "choice.select") {
            const auto index = ParseInteger(argument);
            if (!index || *index < 0 ||
                *index >= static_cast<int>(m_session->VM().Choices().size()))
                return UiFailure(
                    "PXWASM-UI-ACTION-004",
                    "The selected choice no longer exists in this Runtime revision.",
                    "index=" + argument,
                    "Refresh Preview and select a current choice.");
            const auto selected = m_previewSession->SelectChoice(*index);
            if (!selected.accepted)
                return UiFailure(
                    "PXWASM-UI-ACTION-CHOICE-001",
                    selected.diagnostics.empty()
                        ? "Preview could not select the choice."
                        : selected.diagnostics.front().message);
            m_skipMode = false;
            m_autoTimerStartedAtMs = 0;
            return complete(px::Status::Ok());
        }
        if (action == "save.open")
            return complete(OpenPreviewOverlay("save"));
        if (action == "load.open")
            return complete(OpenPreviewOverlay("load"));
        if (action == "save.slot") {
            const auto slot = ParseInteger(argument);
            if (!slot || *slot < 0 || *slot >= 6)
                return UiFailure("PXWASM-UI-SAVE-001",
                                 "Preview save slot is invalid.",
                                 "slot=" + argument);
            m_previewSaves.insert_or_assign(*slot,
                                            m_session->CaptureState());
            return complete(
                m_hud.ShowSaveLoad(true, PreviewSaveItems(true)));
        }
        if (action == "load.slot") {
            const auto slot = ParseInteger(argument);
            if (!slot || !m_previewSaves.contains(*slot))
                return UiFailure(
                    "PXWASM-UI-SAVE-002",
                    "This Preview save slot is empty.", "slot=" + argument,
                    "Choose a slot saved during the current Preview session.");
            const auto restored = m_session->RestoreState(
                m_previewSaves.at(*slot), m_runtime.GetClock().NowMs());
            if (!restored) return restored;
            m_paused = false;
            m_autoTimerStartedAtMs = 0;
            const auto shown = m_uiPreviewActive
                                   ? m_hud.ShowTitle()
                                   : m_hud.ShowHUD(CurrentDialogueView());
            if (shown) EmitFocus(true);
            return complete(shown);
        }
        if (action == "settings.open")
            return complete(OpenPreviewOverlay("settings"));
        if (action == "set.bgm.value" || action == "set.se.value" ||
            action == "set.voice.value" || action == "set.speed.value" ||
            action == "set.textscale.value") {
            const auto value = ParseInteger(argument);
            if (!value)
                return UiFailure("PXWASM-UI-SETTINGS-001",
                                 "The Preview setting value is invalid.",
                                 std::string(action) + "=" + argument);
            if (action == "set.bgm.value") {
                m_previewSettings.bgm = std::clamp(*value, 0, 128);
                m_runtime.Audio().SetBGMVolume(m_previewSettings.bgm);
            } else if (action == "set.se.value") {
                m_previewSettings.se = std::clamp(*value, 0, 128);
                m_runtime.Audio().SetSEVolume(m_previewSettings.se);
            } else if (action == "set.voice.value") {
                m_previewSettings.voice = std::clamp(*value, 0, 128);
                m_runtime.Audio().SetVoiceVolume(m_previewSettings.voice);
            } else if (action == "set.speed.value") {
                m_previewSettings.textSpeedMs = std::clamp(*value, 0, 120);
                m_session->VM().SetDefaultTextSpeed(
                    m_previewSettings.textSpeedMs);
            } else {
                m_previewSettings.textScale =
                    std::clamp(*value / 100.0f, 0.75f, 2.0f);
            }
            return complete(px::Status::Ok());
        }
        if (action == "set.bgm.up" || action == "set.bgm.down" ||
            action == "set.se.up" || action == "set.se.down" ||
            action == "set.voice.up" || action == "set.voice.down" ||
            action == "set.speed.up" || action == "set.speed.down") {
            const int direction = action.ends_with(".up") ? 1 : -1;
            if (action.starts_with("set.bgm.")) {
                m_previewSettings.bgm = std::clamp(
                    m_previewSettings.bgm + direction * 8, 0, 128);
                m_runtime.Audio().SetBGMVolume(m_previewSettings.bgm);
            } else if (action.starts_with("set.se.")) {
                m_previewSettings.se = std::clamp(
                    m_previewSettings.se + direction * 8, 0, 128);
                m_runtime.Audio().SetSEVolume(m_previewSettings.se);
            } else if (action.starts_with("set.voice.")) {
                m_previewSettings.voice = std::clamp(
                    m_previewSettings.voice + direction * 8, 0, 128);
                m_runtime.Audio().SetVoiceVolume(m_previewSettings.voice);
            } else {
                m_previewSettings.textSpeedMs = std::clamp(
                    m_previewSettings.textSpeedMs + direction * 4, 0, 120);
                m_session->VM().SetDefaultTextSpeed(
                    m_previewSettings.textSpeedMs);
            }
            const auto shown = m_hud.ShowSettings(m_previewSettings);
            return complete(shown);
        }
        if (action == "set.skipread.value" ||
            action == "set.highcontrast.value" ||
            action == "set.reducedmotion.value") {
            const auto value = ParseBoolean(argument);
            if (!value)
                return UiFailure("PXWASM-UI-SETTINGS-002",
                                 "The Preview setting toggle is invalid.",
                                 std::string(action) + "=" + argument);
            if (action == "set.skipread.value")
                m_previewSettings.skipReadOnly = *value;
            else if (action == "set.highcontrast.value")
                m_previewSettings.highContrast = *value;
            else
                m_previewSettings.reducedMotion = *value;
            return complete(px::Status::Ok());
        }
        if (action == "set.skipread.toggle") {
            m_previewSettings.skipReadOnly =
                !m_previewSettings.skipReadOnly;
            return complete(m_hud.ShowSettings(m_previewSettings));
        }
        if (action == "set.fullscreen.value" ||
            action == "set.fullscreen.toggle")
            return UnsupportedUiCapability(
                action, "window.fullscreen",
                "Studio Preview is embedded in a canvas and cannot change the native window fullscreen state.",
                true,
                "Use Ship > Native Check to verify fullscreen behavior.");
        if (action == "set.selfvoicing.value")
            return UnsupportedUiCapability(
                action, "speech.platform",
                "Platform self-voicing is not available in WASM Preview.",
                true,
                "Use Ship > Native Check to verify platform speech.");
        if (action == "mode.auto") {
            m_autoMode = !m_autoMode;
            if (m_autoMode) m_skipMode = false;
            m_autoTimerStartedAtMs = 0;
            if (!m_uiPreviewActive)
                (void)m_hud.RefreshHUD(CurrentDialogueView());
            return complete(px::Status::Ok());
        }
        if (action == "mode.skip") {
            m_skipMode = !m_skipMode;
            if (m_skipMode) m_autoMode = false;
            m_autoTimerStartedAtMs = 0;
            if (!m_uiPreviewActive)
                (void)m_hud.RefreshHUD(CurrentDialogueView());
            return complete(px::Status::Ok());
        }
        if (action == "backlog.open") {
            return complete(OpenPreviewOverlay("backlog"));
        }
        if (action == "backlog.voice") {
            const auto index = ParseInteger(argument);
            const auto& entries = m_session->Backlog().Entries();
            if (!index || *index < 0 ||
                static_cast<std::size_t>(*index) >= entries.size() ||
                entries[static_cast<std::size_t>(*index)].voice.empty())
                return UiFailure(
                    "PXWASM-UI-BACKLOG-002",
                    "The selected backlog entry has no playable voice.");
            std::string path = entries[static_cast<std::size_t>(*index)].voice;
            if (path.find('/') == std::string::npos)
                path = m_session->VM().Config().voiceDir + path;
            if (!m_runtime.VFS().Exists(path))
                return UiFailure(
                    "PXWASM-UI-BACKLOG-003",
                    "The selected backlog voice is missing from the active Preview asset graph.",
                    path,
                    "Reimport the voice asset or repair its reference.");
            m_runtime.Audio().PlayVoice(path);
            return complete(px::Status::Ok());
        }
        if (action == "backlog.rollback")
            return UiFailure(
                "PXWASM-UI-BACKLOG-004",
                "Backlog rollback is not exposed until the Preview checkpoint history is available.",
                "capability=preview.checkpointHistory; nativeCheckAvailable=true",
                "Use Writer history navigation or Ship > Native Check.");
        if (action == "gallery.open") {
            return complete(OpenPreviewOverlay("gallery"));
        }
        if (action == "cg.view") {
            if (argument.empty() || !m_runtime.VFS().Exists(argument))
                return UiFailure(
                    "PXWASM-UI-GALLERY-002",
                    "The selected gallery image is missing from Preview MEMFS.",
                    argument,
                    "Repair the asset reference and restart Preview.");
            m_session->Stage().SetBackground(argument, false);
            m_uiPreviewActive = false;
            return complete(m_hud.ShowHUD(CurrentDialogueView()));
        }
        if (action == "game.start") {
            m_uiPreviewActive = false;
            m_paused = false;
            m_autoTimerStartedAtMs = 0;
            const auto routed = m_session->Routes().Replace("hud");
            return complete(routed ? m_hud.ShowHUD(CurrentDialogueView())
                                   : routed);
        }
        if (action == "overlay.close") {
            auto routeState = m_session->Routes().CaptureState();
            if (!routeState.modals.empty()) {
                const auto closed = m_session->Routes().CloseModal();
                if (!closed) return closed;
            }
            const std::string target =
                m_session->Routes().CurrentModalRoute().empty()
                    ? std::string(m_session->Routes().CurrentRoute())
                    : std::string(m_session->Routes().CurrentModalRoute());
            const auto shown = target.empty()
                                   ? (m_uiPreviewActive
                                          ? m_hud.ShowTitle()
                                          : m_hud.ShowHUD(CurrentDialogueView()))
                                   : PresentPreviewScreen(target);
            return complete(shown);
        }
        if (action == "hud.toolbar.pin")
            return complete(m_hud.ToggleHudToolbarPinned());
        if (action == "app.quit")
            return UnsupportedUiCapability(
                action, "window.lifecycle",
                "Studio Preview cannot close the Studio process.", true,
                "Use Ship > Native Check to verify application quit behavior.");

        return UiFailure(
            "PXWASM-UI-ACTION-005",
            "This built-in UI action has no WASM Preview handler: " +
                std::string(action),
            "action=" + std::string(action),
            "Use a supported typed Action or verify it with Native Check.");
    }

    template <typename TResult>
    void EmitStatusDiagnostics(const TResult& status,
                               const std::string_view fallbackCode) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : status.Diagnostics()) {
            diagnostics.push_back(
                {{"severity", "error"},
                 {"code", diagnostic.code.empty()
                              ? std::string(fallbackCode)
                              : diagnostic.code},
                 {"category", diagnostic.category.empty()
                                  ? "Preview.Script"
                                  : diagnostic.category},
                 {"message", diagnostic.message},
                 {"details", diagnostic.details},
                 {"source",
                  {{"documentId", m_protocol.DocumentId()},
                   {"blockId", diagnostic.source.nodeId},
                   {"path", diagnostic.source.path},
                   {"property", diagnostic.source.property},
                   {"line", diagnostic.source.line}}}});
        }
        if (diagnostics.empty()) {
            diagnostics.push_back(
                {{"severity", "error"},
                 {"code", fallbackCode},
                 {"category", "Preview.Script"},
                 {"message", "WASM script Runtime rejected this operation."}});
        }
        m_protocol.Emit("diagnostics",
                        Json{{"diagnostics", diagnostics}}.dump());
    }

    bool InstallRuntimeFiles(const std::string& filesJson) {
        if (!m_scriptHost) return false;
        if (m_runtimeFilesInstalled) {
            if (filesJson == m_runtimeFilesJson) return true;
            EmitControlDiagnostic(
                "PXWASM-SCRIPT-002",
                "The active script extension graph changed. Restart Preview before applying this revision.");
            return false;
        }
        const Json files = Json::parse(filesJson, nullptr, false);
        if (files.is_discarded() || !files.is_array()) {
            EmitControlDiagnostic(
                "PXWASM-SCRIPT-003",
                "The active script extension file contract is malformed.");
            return false;
        }
        std::vector<std::string> manifests;
        for (const auto& file : files) {
            if (!file.is_object() ||
                file.value("kind", std::string{}) != "extensionManifest")
                continue;
            std::string path = file.value("virtualPath", std::string{});
            constexpr std::string_view projectPrefix = "/project/";
            if (!path.starts_with(projectPrefix) ||
                !path.ends_with(".pxextension")) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-004",
                    "A Preview extension manifest is outside the allowlisted project VFS root.");
                return false;
            }
            path.erase(0, projectPrefix.size());
            if (!path.starts_with("Content/Extensions/") ||
                path.find("..") != std::string::npos ||
                !m_runtime.VFS().Exists(path)) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-005",
                    "A declared Preview extension manifest is missing from MEMFS.");
                return false;
            }
            manifests.push_back(std::move(path));
        }
        std::ranges::sort(manifests);
        if (std::ranges::adjacent_find(manifests) != manifests.end()) {
            EmitControlDiagnostic(
                "PXWASM-SCRIPT-006",
                "The active Preview extension graph contains duplicate manifests.");
            return false;
        }
        for (const auto& manifest : manifests) {
            if (!m_scriptHost->LoadExtensionManifest(manifest)) {
                EmitControlDiagnostic(
                    "PXWASM-SCRIPT-007",
                    "A typed script extension failed validation or registration: " +
                        manifest);
                return false;
            }
        }
        m_scriptHost->Emit("engine.ready");
        m_runtimeFilesJson = filesJson;
        m_runtimeFilesInstalled = true;
        return true;
    }

    void EmitControlDiagnostic(const std::string_view code,
                               const std::string_view message) {
        m_protocol.Emit(
            "diagnostics",
            Json{{"diagnostics",
                  Json::array({{{"severity", "error"},
                                {"code", code},
                                {"category", "Preview.Debugger"},
                                {"message", message},
                                {"suggestion",
                                 "Apply the current committed revision and retry the debugger command."},
                                {"source",
                                 {{"documentId", m_protocol.DocumentId()},
                                  {"blockId", m_session
                                                  ? m_session->VM().CurrentSourceId()
                                                  : std::string{}},
                                  {"line", m_session
                                               ? m_session->VM().CurrentSourceLine()
                                               : 0}}}}})}}
                .dump());
    }

    Json RuntimeSnapshotPayload(
        const px::RuntimeSession::GameState& state) const {
        const auto trackPayload = [](const px::audio::AudioEngine::TrackState& track) {
            return Json{{"path", track.path},
                        {"loop", track.loop},
                        {"playing", track.playing},
                        {"playbackFrame", track.playbackFrame}};
        };
        Json routes = {{"stack", Json::array()},
                       {"modals", Json::array()},
                       {"stackCount", state.routes.stack.size()},
                       {"modalCount", state.routes.modals.size()},
                       {"truncated",
                        state.routes.stack.size() > kRuntimeSnapshotItemLimit ||
                            state.routes.modals.size() > kRuntimeSnapshotItemLimit}};
        for (std::size_t index = 0;
             index < std::min(state.routes.stack.size(), kRuntimeSnapshotItemLimit);
             ++index)
            routes["stack"].push_back(state.routes.stack[index]);
        for (std::size_t index = 0;
             index < std::min(state.routes.modals.size(), kRuntimeSnapshotItemLimit);
             ++index)
            routes["modals"].push_back(state.routes.modals[index]);

        Json playbacks = Json::array();
        for (std::size_t index = 0;
             index < std::min(state.timelines.size(), kRuntimeSnapshotItemLimit);
             ++index) {
            const auto& playback = state.timelines[index];
            playbacks.push_back({{"handle", playback.handle},
                                 {"clipId", playback.clip.ToString()},
                                 {"position", playback.position},
                                 {"speed", playback.speed},
                                 {"loopIteration", playback.loopIteration},
                                 {"playing", playback.playing},
                                 {"awaiting", playback.awaiting}});
        }

        const auto& behavior = state.behavior;
        Json fibers = Json::array();
        for (std::size_t index = 0;
             index < std::min(behavior.fibers.size(), kRuntimeSnapshotItemLimit);
             ++index) {
            const auto& fiber = behavior.fibers[index];
            fibers.push_back(
                {{"id", fiber.id},
                 {"entryNodeId", fiber.entry.Empty()
                                     ? Json(nullptr)
                                     : Json(fiber.entry.ToString())},
                 {"currentNodeId", fiber.current.Empty()
                                       ? Json(nullptr)
                                       : Json(fiber.current.ToString())},
                 {"continuationDepth", fiber.continuation.size()},
                 {"delayRemaining", fiber.delayRemaining},
                 {"actionExecution", fiber.actionExecution},
                 {"animationHandle", fiber.animationHandle}});
        }
        Json actions = Json::array();
        for (std::size_t index = 0;
             index < std::min(behavior.actions.size(), kRuntimeSnapshotItemLimit);
             ++index) {
            const auto& action = behavior.actions[index];
            actions.push_back({{"execution", action.execution},
                               {"action", action.invocation.action},
                               {"providerId", action.providerId},
                               {"providerHandle", action.providerHandle}});
        }
        const std::size_t activeTrackCount =
            static_cast<std::size_t>(state.audio.music.playing) +
            static_cast<std::size_t>(state.audio.voice.playing) +
            static_cast<std::size_t>(state.audio.ambience.playing);
        return {{"schemaRevision", 1},
                {"stage",
                 {{"background", state.stage.background},
                  {"previousBackground", state.stage.previousBackground},
                  {"backgroundFade", state.stage.backgroundFade},
                  {"camera",
                   {{"x", state.stage.cameraX},
                    {"y", state.stage.cameraY},
                    {"zoom", state.stage.cameraZoom}}},
                  {"shakeActive", state.stage.shakeRemaining > 0.0f},
                  {"screenEffectCount", state.stage.screenEffects.size()},
                  {"actorCount", state.stage.actors.size()},
                  {"layerCount", state.stage.layers.size()},
                  {"tweenCount", state.stage.tweens.size()}}},
                {"audio",
                 {{"activeTrackCount", activeTrackCount},
                  {"pendingMusicLoop", state.audio.pendingMusicLoop},
                  {"mainVolume", state.audio.mainVolume},
                  {"musicVolume", state.audio.musicVolume},
                  {"voiceVolume", state.audio.voiceVolume},
                  {"sfxVolume", state.audio.sfxVolume},
                  {"ambienceVolume", state.audio.ambienceVolume},
                  {"music", trackPayload(state.audio.music)},
                  {"voice", trackPayload(state.audio.voice)},
                  {"ambience", trackPayload(state.audio.ambience)}}},
                {"timelines",
                 {{"playbackCount", state.timelines.size()},
                  {"truncated",
                   state.timelines.size() > kRuntimeSnapshotItemLimit},
                  {"playbacks", std::move(playbacks)}}},
                {"routes", std::move(routes)},
                {"behavior",
                 {{"source", "runtime"},
                  {"fiberCount", behavior.fibers.size()},
                  {"actionCount", behavior.actions.size()},
                  {"fibersTruncated",
                   behavior.fibers.size() > kRuntimeSnapshotItemLimit},
                  {"actionsTruncated",
                   behavior.actions.size() > kRuntimeSnapshotItemLimit},
                  {"fibers", std::move(fibers)},
                  {"actions", std::move(actions)}}}};
    }

    void EmitDebugControlResult(
        const std::string_view action, const bool includeSnapshot,
        Json breakpointLines = nullptr,
        Json unresolvedBreakpoints = nullptr,
        Json extra = Json::object()) {
        if (!m_session) return;
        Json event = {{"kind", "controlResult"},
                      {"action", action},
                      {"state", action == "stop"
                                    ? std::string_view{"stopped"}
                                    : StateName(m_session->VM().State())},
                      {"programCounter", m_session->VM().ProgramCounter()},
                      {"sourceLine", m_session->VM().CurrentSourceLine()},
                      {"sourceId", m_session->VM().CurrentSourceId()},
                      {"sourceDocumentId", m_protocol.DocumentId()},
                      {"script", m_session->VM().CurrentScript()},
                      {"choiceCount", m_session->VM().Choices().size()},
                      {"variables", nullptr},
                      {"callStack", nullptr},
                      {"runtimeSnapshot", nullptr},
                      {"breakpointLines", std::move(breakpointLines)},
                      {"unresolvedBreakpoints",
                       std::move(unresolvedBreakpoints)},
                      {"scriptBreakpoints", nullptr},
                      {"unresolvedScriptBreakpoints", nullptr},
                      {"scriptBackend", nullptr},
                      {"scriptPaused", nullptr},
                      {"scriptPauseReason", nullptr},
                      {"scriptCallStack", nullptr},
                      {"scriptWatches", nullptr}};
        if (m_scriptHost) {
            const auto& script = m_scriptHost->CaptureDebugState();
            event["scriptBackend"] = m_scriptHost->BackendId();
            event["scriptPaused"] = script.paused;
            event["scriptPauseReason"] = script.reason;
            event["scriptCallStack"] = Json::array();
            for (const auto& frame : script.frames) {
                Json locals = Json::array();
                for (const auto& local : frame.locals)
                    locals.push_back(
                        {{"name", local.name}, {"value", local.value}});
                event["scriptCallStack"].push_back(
                    {{"source", frame.source},
                     {"function", frame.function},
                     {"line", frame.line},
                     {"locals", std::move(locals)}});
            }
        }
        if (extra.is_object()) {
            for (auto entry = extra.begin(); entry != extra.end(); ++entry)
                event[entry.key()] = entry.value();
        }
        if (includeSnapshot) {
            const auto state = m_session->CaptureState();
            event["variables"] = Json::object();
            for (const auto& [name, value] : state.variables)
                event["variables"][name] = value;
            event["callStack"] = Json::array();
            for (const auto& frame : state.vm.callStack)
                event["callStack"].push_back(
                    {{"script", frame.script},
                     {"programCounter", frame.pc}});
            event["runtimeSnapshot"] = RuntimeSnapshotPayload(state);
        }
        m_protocol.Emit("debug", event.dump());
    }

    void EmitScriptDebugStateIfChanged() {
        if (!m_scriptHost || !m_session) return;
        const auto& script = m_scriptHost->CaptureDebugState();
        std::string signature = script.paused ? "paused:" : "running:";
        signature += script.reason;
        for (const auto& frame : script.frames) {
            signature += ":" + frame.source + ":" + frame.function +
                         ":" + std::to_string(frame.line);
            for (const auto& local : frame.locals)
                signature += ":" + local.name + "=" + local.value;
        }
        if (signature == m_lastScriptDebugSignature) return;
        m_lastScriptDebugSignature = std::move(signature);

        Json frames = Json::array();
        for (const auto& frame : script.frames) {
            Json locals = Json::array();
            for (const auto& local : frame.locals)
                locals.push_back(
                    {{"name", local.name}, {"value", local.value}});
            frames.push_back({{"source", frame.source},
                              {"function", frame.function},
                              {"line", frame.line},
                              {"locals", std::move(locals)}});
        }
        Json breakpoints = Json::array();
        for (const auto& breakpoint : m_scriptBreakpoints)
            breakpoints.push_back(
                {{"source", breakpoint.source}, {"line", breakpoint.line}});
        m_protocol.Emit(
            "debug",
            Json{{"kind", "scriptState"},
                 {"scope", "debugger"},
                 {"state", StateName(m_session->VM().State())},
                 {"scriptBackend", m_scriptHost->BackendId()},
                 {"scriptPaused", script.paused},
                 {"scriptPauseReason", script.reason},
                 {"scriptCallStack", std::move(frames)},
                 {"scriptBreakpoints", std::move(breakpoints)},
                 {"unresolvedScriptBreakpoints", Json::array()},
                 {"scriptWatches", Json::array()}}
                .dump());
    }

    void EmitFocus(const bool force) {
        if (!m_session) return;
        const std::string state(StateName(m_session->VM().State()));
        if (state != m_lastVmState) {
            m_lastVmState = state;
            m_protocol.Emit(
                "state",
                Json{{"status", state},
                     {"mode", "wasm"},
                     {"reason", "vmTransition"}}
                    .dump());
        }
        const auto& code = m_session->VM().CurrentProgram().code;
        int operationIndex = m_session->VM().ProgramCounter();
        if (m_operationFocusOverride && *m_operationFocusOverride >= 0 &&
            *m_operationFocusOverride < static_cast<int>(code.size())) {
            operationIndex = *m_operationFocusOverride;
        } else if (m_session->VM().State() == px::vn::VMState::WaitingClick &&
                   operationIndex > 0) {
            --operationIndex;
        }
        if (operationIndex < 0 || operationIndex >= static_cast<int>(code.size()))
            operationIndex = -1;
        const std::string sourceId = operationIndex >= 0
            ? code[static_cast<std::size_t>(operationIndex)].sourceId
            : std::string{};
        const int line = operationIndex >= 0
            ? code[static_cast<std::size_t>(operationIndex)].line
            : 0;
        const std::string signature = state + ":" + sourceId + ":" +
                                      std::to_string(line) + ":" +
                                      std::to_string(operationIndex);
        if (!force && signature == m_lastFocus) return;
        m_lastFocus = signature;
        m_protocol.Emit(
            "runtimeFocus",
            Json{{"vmState", state},
                 {"blockId", sourceId},
                 {"line", line},
                 {"operation", operationIndex >= 0 ? operationIndex + 1 : 0},
                 {"operationIndex", operationIndex},
                 {"operationTotal", code.size()}}
                .dump());
    }

    void DispatchPerformanceEvents(const double fromTime,
                                   const double toTime) {
        if (!m_performanceSequence) return;
        for (const auto& event :
             m_performanceSequence->EventsBetween(fromTime, toTime)) {
            px::ui::ActionContext context;
            context.sourceScene = "performance://" +
                                  m_performanceSequence->sceneId;
            context.sourceNode = px::Uuid::Parse(event.id).value_or(
                px::Uuid::FromName("PrismatiX.Performance.Event/" +
                                   event.id));
            context.signal = "timeline.event";
            context.preview = true;
            const px::Status status = m_hud.Actions().Dispatch(
                {event.actionId, event.arguments, std::move(context)},
                {.previewSafeMode = true});
            if (status) continue;
            Json diagnostics = Json::array();
            for (const auto& diagnostic : status.Diagnostics()) {
                diagnostics.push_back(
                    {{"severity", "error"},
                     {"code", diagnostic.code},
                     {"category", diagnostic.category},
                     {"message", diagnostic.message},
                     {"details", diagnostic.details},
                     {"source",
                      {{"documentId", m_performanceSequence->sceneId},
                       {"nodeId", event.id},
                       {"property", "payload.actionId"}}}});
            }
            m_protocol.Emit("diagnostics",
                            Json{{"diagnostics", diagnostics}}.dump());
        }
    }

    bool ApplyPerformanceUi(const px::preview::PerformancePreviewPlan& plan) {
        const px::Status status =
            px::preview::ApplyPerformancePreviewUiPlan(m_hud, plan);
        if (status) return true;
        const std::string sourceId = plan.uiAnimation
                                         ? plan.uiAnimation->clipId
                                     : !plan.uiControls.empty()
                                         ? plan.uiControls.front().clipId
                                         : std::string{};
        Json diagnostics = Json::array();
        for (const auto& diagnostic : status.Diagnostics()) {
            diagnostics.push_back(
                {{"severity", "error"},
                 {"code", diagnostic.code},
                 {"category", diagnostic.category},
                 {"message", diagnostic.message},
                 {"details", diagnostic.details},
                 {"capability", "timeline.ui"},
                 {"nativeCheckAvailable", true},
                 {"suggestion", "Choose a control or animation UUID from the bound UI scene, then apply Preview again."},
                 {"source",
                  {{"documentId", plan.sceneId},
                   {"nodeId", sourceId},
                   {"property", "payload.targetId"}}}});
        }
        m_protocol.Emit("diagnostics",
                        Json{{"diagnostics", diagnostics}}.dump());
        return false;
    }

    void SetPerformanceAudioPaused(const bool paused) {
        if (!m_session || !m_performanceSequence) return;
        const auto plan = m_performanceSequence->Sample(m_performanceTime);
        auto& audio = m_session->Audio();
        if (plan.audio.music.managed && plan.audio.music.playing)
            (void)audio.SetMusicPaused(paused);
        if (plan.audio.ambience.managed && plan.audio.ambience.playing)
            (void)audio.SetAmbiencePaused(paused);
        if (plan.audio.voice.managed && plan.audio.voice.playing)
            (void)audio.SetVoicePaused(paused);
        m_protocol.Emit(
            "audioState",
            Json{{"status", paused ? "paused" : "running"},
                 {"source", "performance"}}
                .dump());
    }

    void StopPerformanceAudio() {
        if (!m_session || !m_performanceSequence) return;
        const auto plan = m_performanceSequence->Sample(m_performanceTime);
        auto& audio = m_session->Audio();
        if (plan.audio.music.managed) audio.StopBGM(0);
        if (plan.audio.ambience.managed) audio.StopAmbience(0);
        if (plan.audio.voice.managed) audio.StopVoice();
        m_protocol.Emit(
            "audioState",
            Json{{"status", "stopped"}, {"source", "performance"}}
                .dump());
    }

    px::preview::PreviewProtocolV2 m_protocol;
    px::Runtime m_runtime;
    std::unique_ptr<px::RuntimeSession> m_session;
    std::unique_ptr<px::sdk::PreviewSession> m_previewSession;
    px::ui::GalgameUI m_hud;
    std::vector<std::string> m_choices;
    std::string m_lastFocus;
    std::string m_lastVmState;
    std::optional<int> m_operationFocusOverride;
    std::string m_drained = "[]";
    std::uint64_t m_performanceRevision = 0;
    std::optional<px::preview::PerformancePreviewSequence>
        m_performanceSequence;
    double m_performanceTime = 0.0;
    double m_lastPerformanceEventTime = 0.0;
    std::string m_performanceAudioSignature;
    px::vn::GameCatalog m_catalog;
    std::string m_projectManifestJson;
    std::string m_runtimeIrJson;
    std::string m_performanceJson;
    std::string m_lastUiAction;
    std::string m_lastScriptDebugSignature;
    px::script::ScriptServices m_scriptServices;
    std::unique_ptr<px::script::ScriptHost> m_scriptHost;
    std::vector<px::script::DebugBreakpoint> m_scriptBreakpoints;
    std::string m_runtimeFilesJson = "[]";
    std::unordered_map<int, px::RuntimeSession::GameState> m_previewSaves;
    std::set<std::string> m_registeredRoutes;
    px::ui::SettingsPresentation m_previewSettings;
    bool m_runtimeFilesInstalled = false;
    bool m_ready = false;
    bool m_paused = false;
    bool m_stopped = false;
    bool m_visible = true;
    bool m_uiPreviewActive = false;
    bool m_autoMode = false;
    bool m_skipMode = false;
    std::uint64_t m_autoTimerStartedAtMs = 0;
    std::optional<px::Vec2> m_pendingPointerClick;
    px::Vec2 m_lastInjectedPointer{};
    bool m_releaseInjectedPointer = false;
};

WasmPreview* Instance(const std::uintptr_t handle) {
    return reinterpret_cast<WasmPreview*>(handle);
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE std::uintptr_t px_preview_create(const char* optionsUtf8) {
    return reinterpret_cast<std::uintptr_t>(new WasmPreview(optionsUtf8));
}

EMSCRIPTEN_KEEPALIVE int px_preview_apply(const std::uintptr_t handle,
                                         const char* envelopeUtf8) {
    return Instance(handle) ? Instance(handle)->Apply(envelopeUtf8, false) : 0;
}

EMSCRIPTEN_KEEPALIVE int px_preview_patch(const std::uintptr_t handle,
                                         const char* envelopeUtf8) {
    return Instance(handle) ? Instance(handle)->Apply(envelopeUtf8, true) : 0;
}

EMSCRIPTEN_KEEPALIVE int px_preview_control(const std::uintptr_t handle,
                                           const char* envelopeUtf8) {
    return Instance(handle) ? Instance(handle)->Control(envelopeUtf8) : 0;
}

EMSCRIPTEN_KEEPALIVE int px_preview_resize(const std::uintptr_t handle,
                                          const int cssWidth,
                                          const int cssHeight,
                                          const double devicePixelRatio,
                                          const int visible) {
    return Instance(handle)
               ? Instance(handle)->Resize(cssWidth, cssHeight,
                                          devicePixelRatio, visible != 0)
               : 0;
}

EMSCRIPTEN_KEEPALIVE int px_preview_tick(const std::uintptr_t handle,
                                        const double nowMs) {
    return Instance(handle) ? Instance(handle)->Tick(nowMs) : 0;
}

EMSCRIPTEN_KEEPALIVE const char* px_preview_drain_events(
    const std::uintptr_t handle) {
    return Instance(handle) ? Instance(handle)->DrainEvents() : "[]";
}

EMSCRIPTEN_KEEPALIVE void px_preview_destroy(const std::uintptr_t handle) {
    delete Instance(handle);
}

}  // extern "C"

int main() { return 0; }
