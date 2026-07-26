#include "Engine/Runtime.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/StudioUi.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/UI/Actions/BuiltInActionProvider.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/StudioUiAdapter.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using Json = nlohmann::json;
constexpr std::string_view kProtocol = "PrismatiXPreviewProtocol";
constexpr std::uint32_t kSchemaRevision = 1;

struct RequestQueue {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Json> requests;
    bool inputClosed = false;
};

Json Response(const std::string& requestId, const std::string& type) {
    return Json{{"protocol", kProtocol},
                {"schemaRevision", kSchemaRevision},
                {"type", type},
                {"requestId", requestId}};
}

Json Error(const std::string& requestId, const std::string& code,
           const std::string& message) {
    Json response = Response(requestId, "error");
    response["code"] = code;
    response["message"] = message;
    return response;
}

void Write(const Json& response) {
    std::cout << response.dump() << '\n' << std::flush;
}

std::optional<std::string> RequiredString(const Json& request, const char* key) {
    const auto found = request.find(key);
    if (found == request.end() || !found->is_string() || found->empty()) return std::nullopt;
    return found->get<std::string>();
}

std::string StatusMessage(const px::Status& status,
                          const std::string_view fallback) {
    if (status.Diagnostics().empty()) return std::string(fallback);
    const auto& diagnostic = status.Diagnostics().front();
    return diagnostic.code + ": " + diagnostic.message;
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code error;
    const auto relative = std::filesystem::relative(candidate, root, error);
    if (error || relative.empty() || relative.is_absolute()) return false;
    return *relative.begin() != "..";
}

std::optional<std::string> SafeProjectRelativePath(const std::string& value) {
    const std::filesystem::path path(value);
    if (path.empty() || path.is_absolute()) return std::nullopt;
    for (const auto& part : path) {
        if (part == "..") return std::nullopt;
    }
    return path.generic_string();
}

bool IsProjectRegularFile(const std::filesystem::path& root,
                          const std::string& relativePath) {
    std::error_code error;
    const auto candidate = std::filesystem::canonical(root / relativePath, error);
    return !error && IsWithin(root, candidate) &&
           std::filesystem::is_regular_file(candidate, error) && !error;
}

std::optional<std::pair<std::filesystem::path, std::string>> ReadProjectFile(
    const std::string& projectRoot, const std::string& relativePath) {
    std::error_code error;
    const auto root = std::filesystem::canonical(projectRoot, error);
    if (error) return std::nullopt;
    const std::filesystem::path requested(relativePath);
    if (requested.is_absolute()) return std::nullopt;
    const auto file = std::filesystem::canonical(root / requested, error);
    if (error || !IsWithin(root, file)) return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream content;
    content << input.rdbuf();
    return std::pair{root, content.str()};
}

px::ui::DialoguePresentation DialogueView(const px::RuntimeSession& session,
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

class PreviewHost final {
public:
    explicit PreviewHost(RequestQueue& queue) : m_queue(queue) {}

    int Run() {
        while (!m_shutdown) {
            ProcessRequests();
            if (m_shutdown) break;
            if (!m_runtime) {
                std::unique_lock lock(m_queue.mutex);
                m_queue.condition.wait(lock, [this] {
                    return !m_queue.requests.empty() || m_queue.inputClosed;
                });
                if (m_queue.inputClosed && m_queue.requests.empty()) break;
                continue;
            }
            if (!m_runtime->BeginFrame()) break;
            Frame();
            m_runtime->EndFrame();
            SDL_Delay(1);
        }
        m_session.reset();
        m_runtime.reset();
        return 0;
    }

private:
    void ProcessRequests() {
        std::deque<Json> pending;
        {
            std::lock_guard lock(m_queue.mutex);
            pending.swap(m_queue.requests);
        }
        for (const auto& request : pending) Handle(request);
    }

    void Handle(const Json& request) {
        const std::string requestId = request.value("requestId", std::string{});
        if (request.value("protocol", std::string{}) != kProtocol ||
            request.value("schemaRevision", 0U) != kSchemaRevision) {
            Write(Error(requestId, "protocol-mismatch", "Unsupported preview protocol contract"));
            return;
        }
        const auto type = RequiredString(request, "type");
        if (!type) {
            Write(Error(requestId, "missing-type", "Preview request type is required"));
            return;
        }
        if (*type == "hello") {
            Json response = Response(requestId, "ready");
            response["runtimeIrSchemaRevision"] = 1;
            response["studioUiSchemaRevision"] = 1;
            response["renderMode"] = "nativeWindow";
            Write(response);
        } else if (*type == "shutdown") {
            Write(Response(requestId, "shutdownAccepted"));
            m_shutdown = true;
        } else if (*type == "applyRuntimeIr") {
            Apply(request, requestId);
        } else if (*type == "applyUiScene") {
            ApplyUiScene(request, requestId);
        } else if (*type == "seekPerformance") {
            SeekPerformance(request, requestId);
        } else if (*type == "advance") {
            if (m_session) m_session->Advance();
            Write(Response(requestId, "advanceAccepted"));
        } else if (*type == "selectChoice") {
            if (m_session) m_session->SelectChoice(request.value("index", -1));
            Write(Response(requestId, "choiceAccepted"));
        } else {
            Write(Error(requestId, "unknown-request", "Unknown preview request type"));
        }
    }

    void Apply(const Json& request, const std::string& requestId) {
        spdlog::info("Preview apply request received");
        const auto projectRoot = RequiredString(request, "projectRoot");
        const auto documentId = RequiredString(request, "documentId");
        const auto irPath = RequiredString(request, "irPath");
        const auto revision = request.find("committedRevision");
        if (!projectRoot || !documentId || !irPath || revision == request.end() ||
            !revision->is_number_unsigned()) {
            Write(Error(requestId, "invalid-apply-request",
                        "projectRoot, documentId, committedRevision and irPath are required"));
            return;
        }
        const std::uint64_t committedRevision = revision->get<std::uint64_t>();
        if (const auto current = m_appliedRevisions.find(*documentId);
            current != m_appliedRevisions.end() && committedRevision <= current->second) {
            Json response = Response(requestId, "staleRevision");
            response["documentId"] = *documentId;
            response["requestedRevision"] = committedRevision;
            response["appliedRevision"] = current->second;
            Write(response);
            return;
        }
        const auto loaded = ReadProjectFile(*projectRoot, *irPath);
        if (!loaded) {
            Write(Error(requestId, "invalid-ir-path",
                        "Runtime IR must be an existing file inside the project root"));
            return;
        }
        const px::sdk::RuntimeIrParseResult parsed = px::sdk::ParseRuntimeIr(loaded->second);
        spdlog::info("Runtime IR parsed operations={}", parsed.document.operations.size());
        if (!parsed.Valid()) {
            Json response = Response(requestId, "runtimeIrRejected");
            response["documentId"] = *documentId;
            response["requestedRevision"] = committedRevision;
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : parsed.diagnostics) {
                response["diagnostics"].push_back({{"code", diagnostic.code},
                                                    {"message", diagnostic.message},
                                                    {"operationIndex", diagnostic.operationIndex}});
            }
            Write(response);
            return;
        }
        if (parsed.document.documentId != *documentId ||
            parsed.document.committedRevision != committedRevision) {
            Write(Error(requestId, "ir-identity-mismatch",
                        "Runtime IR identity or committed revision does not match the request"));
            return;
        }
        if (!EnsureRuntime(loaded->first)) {
            Write(Error(requestId, "runtime-start-failed", "Native preview runtime could not start"));
            return;
        }
        spdlog::info("Native runtime ready");
        if (!m_session->StartRuntimeIrText(loaded->second, *irPath)) {
            Write(Error(requestId, "runtime-program-rejected",
                        "RuntimeSession rejected the compiled Runtime IR program"));
            return;
        }
        spdlog::info("Runtime IR program started");
        m_showUiPreview = false;
        m_choices.clear();
        (void)m_hud.ShowHUD(DialogueView(*m_session, m_choices));
        m_appliedRevisions[*documentId] = committedRevision;
        Json response = Response(requestId, "runtimeIrApplied");
        response["documentId"] = *documentId;
        response["appliedRevision"] = committedRevision;
        response["operationCount"] = parsed.document.operations.size();
        response["renderMode"] = "nativeWindow";
        Write(response);
    }

    void ApplyUiScene(const Json& request, const std::string& requestId) {
        const auto projectRoot = RequiredString(request, "projectRoot");
        const auto sceneId = RequiredString(request, "sceneId");
        const auto uiPath = RequiredString(request, "uiPath");
        const auto revision = request.find("revision");
        if (!projectRoot || !sceneId || !uiPath || revision == request.end() ||
            !revision->is_number_unsigned()) {
            Write(Error(requestId, "invalid-ui-apply-request",
                        "projectRoot, sceneId, uiPath and revision are required"));
            return;
        }
        const std::uint64_t requestedRevision = revision->get<std::uint64_t>();
        if (const auto current = m_appliedUiRevisions.find(*sceneId);
            current != m_appliedUiRevisions.end() && requestedRevision <= current->second) {
            Json response = Response(requestId, "staleUiRevision");
            response["sceneId"] = *sceneId;
            response["requestedRevision"] = requestedRevision;
            response["appliedRevision"] = current->second;
            Write(response);
            return;
        }
        const auto loaded = ReadProjectFile(*projectRoot, *uiPath);
        const auto project = ReadProjectFile(*projectRoot, "project.pxproject");
        if (!loaded || !project) {
            Write(Error(requestId, "invalid-ui-path",
                        "UI scene and project manifest must exist inside the project root"));
            return;
        }
        const px::sdk::StudioUiParseResult parsed =
            px::sdk::ParseStudioUi(loaded->second);
        if (!parsed.Valid()) {
            Json response = Response(requestId, "studioUiRejected");
            response["sceneId"] = *sceneId;
            response["requestedRevision"] = requestedRevision;
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : parsed.diagnostics) {
                response["diagnostics"].push_back(
                    {{"code", diagnostic.code},
                     {"message", diagnostic.message},
                     {"nodeIndex", diagnostic.nodeIndex}});
            }
            Write(response);
            return;
        }
        if (parsed.document.id != *sceneId ||
            parsed.document.revision != requestedRevision) {
            Write(Error(requestId, "ui-identity-mismatch",
                        "UI scene identity or revision does not match the request"));
            return;
        }
        Json manifest = Json::parse(project->second, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) {
            Write(Error(requestId, "invalid-project-manifest",
                        "Project manifest is not valid JSON"));
            return;
        }
        std::unordered_map<std::string, std::string> assets;
        if (manifest.contains("assets") && manifest["assets"].is_array()) {
            for (const auto& asset : manifest["assets"]) {
                if (!asset.contains("id") || !asset["id"].is_string() ||
                    !asset.contains("source") || !asset["source"].is_string()) continue;
                const auto source =
                    SafeProjectRelativePath(asset["source"].get<std::string>());
                if (source && IsProjectRegularFile(loaded->first, *source))
                    assets.emplace(asset["id"].get<std::string>(), *source);
            }
        }
        if (!EnsureRuntime(loaded->first)) {
            Write(Error(requestId, "runtime-start-failed",
                        "Native preview runtime could not start"));
            return;
        }
        if (const auto status = px::ui::RegisterBuiltinUITypes(); !status) {
            Write(Error(requestId, "ui-type-registry-rejected",
                        StatusMessage(
                            status,
                            "Runtime UI property registry could not start")));
            return;
        }
        const std::uint64_t actionCountBefore = m_uiActionCount;
        auto runtimeTree = px::ui::BuildStudioUiRuntimeTree(
            parsed.document,
            [&assets](const std::string_view assetId) -> std::optional<std::string> {
                const auto found = assets.find(std::string(assetId));
                return found == assets.end()
                           ? std::nullopt
                           : std::optional<std::string>{found->second};
            },
            [this](const px::sdk::StudioUiAction& action) {
                m_lastUiAction = action.id;
                ++m_uiActionCount;
                spdlog::info("Native UI preview action {}", action.id);
            });
        if (!runtimeTree.Valid() || !runtimeTree.unresolvedAssetIds.empty()) {
            Json response = Response(requestId, "studioUiRejected");
            response["sceneId"] = *sceneId;
            response["requestedRevision"] = requestedRevision;
            response["diagnostics"] = Json::array();
            for (const auto& diagnostic : runtimeTree.diagnostics) {
                response["diagnostics"].push_back(
                    {{"code", diagnostic.code},
                     {"message", diagnostic.message},
                     {"nodeId", diagnostic.nodeId}});
            }
            for (const auto& assetId : runtimeTree.unresolvedAssetIds) {
                response["diagnostics"].push_back(
                    {{"code", "PXUISTUDIO2004"},
                     {"message", "UI image asset is not present in the project manifest"},
                     {"assetId", assetId}});
            }
            Write(response);
            return;
        }
        const std::size_t nodeCount = runtimeTree.nodeCount;
        const std::size_t actionBindingCount = runtimeTree.actionBindingCount;
        const std::size_t behaviorNodeCount = runtimeTree.behaviorNodeCount;
        const std::size_t behaviorTriggerCount =
            runtimeTree.behaviorTriggerCount;
        const std::size_t animationClipCount =
            runtimeTree.animationClipCount;
        const std::size_t animationTrackCount =
            runtimeTree.animationTrackCount;
        if (const auto status = m_uiPreview.SetRoot(std::move(runtimeTree.root));
            !status) {
            Write(Error(requestId, "ui-runtime-rejected",
                        StatusMessage(
                            status,
                            "Runtime UIContext rejected the Studio UI tree")));
            return;
        }
        const auto previewAction =
            [this](const px::ui::ActionInvocation& invocation) {
                m_lastUiAction = invocation.action;
                ++m_uiActionCount;
                spdlog::info("Native UI preview behavior action {}",
                             invocation.action);
                return px::Status::Ok();
            };
        if (auto provider =
                std::dynamic_pointer_cast<px::ui::BuiltInActionProvider>(
                    m_uiPreview.Actions().FindProvider("builtin"))) {
            provider->SetFallback(previewAction);
        } else {
            auto createdProvider =
                std::make_shared<px::ui::BuiltInActionProvider>();
            createdProvider->SetFallback(previewAction);
            if (const auto status =
                    m_uiPreview.Actions().RegisterProvider(
                        std::move(createdProvider));
                !status) {
                Write(Error(requestId, "ui-action-provider-rejected",
                            StatusMessage(
                                status,
                                "Runtime UI action provider could not be configured")));
                return;
            }
        }
        if (runtimeTree.animations) {
            for (const auto& clip : runtimeTree.animations->clips) {
                for (const auto& track : clip.tracks) {
                    auto* target = m_uiPreview.Root()
                                       ? m_uiPreview.Root()->Find(track.node)
                                       : nullptr;
                    const auto* property =
                        target ? px::TypeRegistry::Global().FindProperty(
                                     std::string(target->TypeName()),
                                     track.property)
                               : nullptr;
                    if (!target || !property || !property->get ||
                        !property->set) {
                        const std::string targetType =
                            target ? std::string(target->TypeName())
                                   : "<missing>";
                        const std::string propertyState =
                            !property
                                ? "missing"
                                : "get=" +
                                      std::string(property->get ? "yes" : "no") +
                                      ",set=" +
                                      std::string(property->set ? "yes" : "no");
                        Write(Error(
                            requestId, "ui-animation-target-rejected",
                            "Animation target " + track.node.ToString() +
                                " (" + targetType + ")." + track.property +
                                " is not a writable Runtime property (" +
                                propertyState + ")"));
                        return;
                    }
                }
            }
            if (const auto status =
                    m_uiPreview.SetAnimations(
                        std::move(*runtimeTree.animations), true);
                !status) {
                Write(Error(requestId, "ui-animation-rejected",
                            StatusMessage(
                                status,
                                "Runtime rejected the Studio animation library")));
                return;
            }
        }
        if (runtimeTree.behaviorGraph || !runtimeTree.behaviorTriggers.empty()) {
            if (const auto status = m_uiPreview.ConfigureTriggers(
                    std::move(runtimeTree.behaviorTriggers),
                    std::move(runtimeTree.behaviorGraph), *uiPath);
                !status) {
                Write(Error(requestId, "ui-behavior-rejected",
                            StatusMessage(
                                status,
                                "Runtime rejected the Studio Behavior Graph")));
                return;
            }
        }
        m_uiPreview.SetDiagnosticOverlayEnabled(false);
        m_showUiPreview = true;
        m_appliedUiRevisions[*sceneId] = requestedRevision;
        Json response = Response(requestId, "studioUiApplied");
        response["sceneId"] = *sceneId;
        response["appliedRevision"] = requestedRevision;
        response["nodeCount"] = nodeCount;
        response["actionBindingCount"] = actionBindingCount;
        response["behaviorNodeCount"] = behaviorNodeCount;
        response["behaviorTriggerCount"] = behaviorTriggerCount;
        response["animationClipCount"] = animationClipCount;
        response["animationTrackCount"] = animationTrackCount;
        response["actionsDispatchedDuringApply"] =
            m_uiActionCount - actionCountBefore;
        response["renderMode"] = "nativeWindow";
        Write(response);
    }

    void SeekPerformance(const Json& request, const std::string& requestId) {
        const auto projectRoot = RequiredString(request, "projectRoot");
        const auto sceneId = RequiredString(request, "sceneId");
        const auto performancePath = RequiredString(request, "performancePath");
        const auto revision = request.find("revision");
        const auto time = request.find("time");
        if (!projectRoot || !sceneId || !performancePath || revision == request.end() ||
            !revision->is_number_unsigned() || time == request.end() || !time->is_number()) {
            Write(Error(requestId, "invalid-performance-seek",
                        "projectRoot, sceneId, performancePath, revision and time are required"));
            return;
        }
        const auto loaded = ReadProjectFile(*projectRoot, *performancePath);
        const auto project = ReadProjectFile(*projectRoot, "project.pxproject");
        if (!loaded || !project) {
            Write(Error(requestId, "invalid-performance-path",
                        "Performance and project manifest must exist inside the project root"));
            return;
        }
        Json document = Json::parse(loaded->second, nullptr, false);
        Json manifest = Json::parse(project->second, nullptr, false);
        const std::uint64_t requestedRevision = revision->get<std::uint64_t>();
        const double requestedTime = time->get<double>();
        if (document.is_discarded() || !document.is_object() ||
            document.value("format", std::string{}) != "PrismatiXPerformance" ||
            document.value("schemaRevision", 0U) != 1U ||
            document.value("sceneId", std::string{}) != *sceneId ||
            document.value("revision", std::uint64_t{}) != requestedRevision ||
            !std::isfinite(requestedTime) || requestedTime < 0.0 ||
            !document.contains("stage") || !document.contains("timeline")) {
            Write(Error(requestId, "performance-identity-mismatch",
                        "Performance identity, revision, schema, or seek time is invalid"));
            return;
        }
        const double duration = document["timeline"].value("duration", 0.0);
        const double seekTime = std::clamp(requestedTime, 0.0, duration);
        if (!EnsureRuntime(loaded->first)) {
            Write(Error(requestId, "runtime-start-failed", "Native preview runtime could not start"));
            return;
        }

        m_showUiPreview = false;
        std::unordered_map<std::string, std::string> assets;
        if (manifest.is_object() && manifest.contains("assets") && manifest["assets"].is_array()) {
            for (const auto& asset : manifest["assets"]) {
                if (asset.contains("id") && asset["id"].is_string() &&
                    asset.contains("source") && asset["source"].is_string()) {
                    assets[asset["id"].get<std::string>()] = asset["source"].get<std::string>();
                }
            }
        }
        std::unordered_map<std::string, std::unordered_map<std::string, Json>> sampled;
        if (document["timeline"].contains("tracks") && document["timeline"]["tracks"].is_array()) {
            for (const auto& track : document["timeline"]["tracks"]) {
                if (!track.contains("targetId") || !track["targetId"].is_string() ||
                    !track.contains("keyframes") || !track["keyframes"].is_array()) continue;
                const std::string target = track["targetId"].get<std::string>();
                for (const auto& key : track["keyframes"]) {
                    if (key.value("time", duration + 1.0) <= seekTime &&
                        key.contains("property") && key["property"].is_string() &&
                        key.contains("value")) {
                        sampled[target][key["property"].get<std::string>()] = key["value"];
                    }
                }
            }
        }
        auto sampleNumber = [&sampled](const Json& node, const std::string& id,
                                        const char* property, const double fallback) {
            const auto target = sampled.find(id);
            if (target != sampled.end()) {
                const auto value = target->second.find(property);
                if (value != target->second.end() && value->second.is_number())
                    return value->second.get<double>();
            }
            return node.value(property, fallback);
        };

        m_session->Stage().ClearAll();
        std::size_t nodeCount = 0;
        if (document["stage"].contains("nodes") && document["stage"]["nodes"].is_array()) {
            for (const auto& node : document["stage"]["nodes"]) {
                if (!node.value("visible", true)) continue;
                const std::string id = node.value("id", std::string{});
                const std::string kind = node.value("kind", std::string{});
                const std::string assetId = node.value("assetId", std::string{});
                const auto asset = assets.find(assetId);
                if (id.empty() || asset == assets.end()) continue;
                if (kind == "background") {
                    m_session->Stage().SetBackground(asset->second, false);
                } else {
                    const float x = static_cast<float>(sampleNumber(node, id, "x", 0.0));
                    const float y = static_cast<float>(sampleNumber(node, id, "y", 0.0));
                    const float scale = static_cast<float>(sampleNumber(node, id, "scaleX", 1.0));
                    const float opacity = static_cast<float>(sampleNumber(node, id, "opacity", 1.0));
                    m_session->Stage().SetLayer(id, asset->second, x, y, scale,
                        static_cast<std::uint8_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f),
                        node.value("zOrder", 0));
                }
                ++nodeCount;
            }
        }
        Json response = Response(requestId, "performanceSeeked");
        response["sceneId"] = *sceneId;
        response["revision"] = requestedRevision;
        response["time"] = seekTime;
        response["nodeCount"] = nodeCount;
        response["unsafeEventsSkipped"] = 0;
        Write(response);
    }

    bool EnsureRuntime(const std::filesystem::path& projectRoot) {
        if (m_runtime && m_projectRoot == projectRoot) return true;
        m_session.reset();
        m_runtime.reset();
        m_appliedRevisions.clear();
        m_appliedUiRevisions.clear();
        m_showUiPreview = false;
        auto runtime = std::make_unique<px::Runtime>();
        px::RuntimeConfig config;
        config.title = "PrismatiX Preview";
        config.mountDirs = {projectRoot.string()};
        spdlog::info("Initializing native runtime projectRoot={}", projectRoot.string());
        if (!runtime->Init(config)) return false;
        spdlog::info("Constructing runtime session");
        auto session = std::make_unique<px::RuntimeSession>(px::RuntimeSession::Services{
            runtime->VFS(), runtime->Audio(), runtime->Renderer(), runtime->Assets()});
        m_hud.SetActionSink([this](const px::ui::GalgameAction& action) {
            if (action.command == "choice.select" && m_session) {
                m_session->SelectChoice(std::atoi(action.argument.c_str()));
            }
        });
        m_projectRoot = projectRoot;
        m_runtime = std::move(runtime);
        m_session = std::move(session);
        return true;
    }

    void Frame() {
        const float delta = m_runtime->GetClock().DeltaSeconds();
        const std::uint64_t now = m_runtime->GetClock().NowMs();
        if (m_showUiPreview) {
            auto& input = m_runtime->GetInput();
            int width = 0;
            int height = 0;
            m_runtime->Renderer().GetLogicalSize(width, height);
            (void)m_uiPreview.Update(input, width, height, delta);
            m_uiPreview.Render(m_runtime->Renderer());
            return;
        }
        m_session->Update(now, delta);
        m_choices.clear();
        if (m_session->VM().State() == px::vn::VMState::WaitingChoice) {
            for (const auto& choice : m_session->VM().Choices()) m_choices.push_back(choice.text);
        }
        (void)m_hud.RefreshHUD(DialogueView(*m_session, m_choices));
        auto& input = m_runtime->GetInput();
        int width = 0;
        int height = 0;
        m_runtime->Renderer().GetLogicalSize(width, height);
        const bool consumed = m_hud.Update(input, width, height, delta);
        if (!consumed && input.LeftClick() &&
            m_session->VM().State() != px::vn::VMState::WaitingChoice) {
            m_session->Advance();
        }
        m_session->Stage().Render();
        m_hud.Render(m_runtime->Renderer());
    }

    RequestQueue& m_queue;
    std::unique_ptr<px::Runtime> m_runtime;
    std::unique_ptr<px::RuntimeSession> m_session;
    px::ui::GalgameUI m_hud;
    px::ui::UIContext m_uiPreview;
    std::filesystem::path m_projectRoot;
    std::unordered_map<std::string, std::uint64_t> m_appliedRevisions;
    std::unordered_map<std::string, std::uint64_t> m_appliedUiRevisions;
    std::vector<std::string> m_choices;
    std::string m_lastUiAction;
    std::uint64_t m_uiActionCount = 0;
    bool m_showUiPreview = false;
    bool m_shutdown = false;
};

void ReadRequests(RequestQueue& queue) {
    std::string line;
    while (std::getline(std::cin, line)) {
        Json request = Json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            request = Json{{"protocol", kProtocol},
                           {"schemaRevision", kSchemaRevision},
                           {"type", "invalidJson"}};
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
    }
    queue.condition.notify_one();
}

}  // namespace

int main() {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("PrismatiXPreviewHost", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));

    RequestQueue queue;
    std::jthread reader([&queue] { ReadRequests(queue); });
    PreviewHost host(queue);
    return host.Run();
}
