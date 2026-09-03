#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/UIRuntimeState.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <set>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace px {

// The authoritative gameplay assembly used by both the shipped Player and the
// Editor Preview. Hosts provide platform/render services and only inject
// diagnostics, hot reload, and product-specific UI callbacks around it.
class RuntimeSession {
public:
    struct Services {
        io::VFS& vfs;
        audio::AudioEngine& audio;
        graphics::Renderer2D& renderer;
        graphics::AssetCache& assets;
    };

    struct GameState {
        vn::VMRuntimeState vm;
        // Memory-applied Runtime IR has no VFS source to reload. Checkpoints
        // retain the compiled program so restore is transport-independent.
        std::shared_ptr<const vn::Program> runtimeProgram;
        vn::DialogueSnapshot dialogue;
        std::unordered_map<std::string, int> variables;
        std::unordered_map<std::string, vn::Value> typedVariables;
        vn::Stage::RuntimeState stage;
        audio::AudioEngine::RuntimeState audio;
        std::vector<vn::BacklogEntry> backlog;
        ui::RouteState routes;
        std::vector<animation::PlaybackState> timelines;
        std::vector<animation::AnimationClip> animationClips;
        ui::UIRuntimeState ui;
        std::uint64_t playtimeMs = 0;
    };

    struct PreparedRestore {
        GameState state;
        std::uint64_t nowMs = 0;
    };

    explicit RuntimeSession(Services services);

    bool StartScenario(const std::string& scriptPath, bool resetVariables = true);
    bool StartScenarioText(std::string_view text, const std::string& scriptPath,
                           bool resetVariables = true);
    bool StartRuntimeIr(const std::string& sourcePath,
                        bool resetVariables = true);
    bool StartRuntimeIrText(std::string_view text, const std::string& sourcePath,
                            bool resetVariables = true);
    [[nodiscard]] std::shared_ptr<const vn::Program> PrepareRuntimeIr(
        const std::string& sourcePath);
    Status LoadSourceMap(const std::string& sourcePath);
    Status SetSourceMapText(std::string_view text,
                            const std::string& sourcePath);
    // Commits an already strictly parsed source map. Used with transactional
    // program replacement so no fallible parsing remains after state commit.
    void CommitSourceMap(sdk::SourceMapDocument document,
                         std::string sourcePath) {
        m_sourceMap = std::move(document);
        m_sourceMapPath = std::move(sourcePath);
    }
    [[nodiscard]] std::shared_ptr<const vn::Program> RuntimeProgramIdentity() const {
        return m_runtimeProgramIdentity;
    }
    vn::ProgramPatchStatus PatchRuntimeIrText(std::string_view text,
                                              const std::string& sourcePath);
    vn::ProgramSeekStatus SeekRuntimeIrOperation(
        std::string_view text, const std::string& sourcePath,
        int operationIndex);
    [[nodiscard]] const std::vector<diag::Diagnostic>& LastStartDiagnostics() const {
        return m_lastStartDiagnostics;
    }
    void Update(std::uint64_t nowMs, float deltaSeconds);
    void Advance();
    void SelectChoice(int index);

    [[nodiscard]] GameState CaptureState(std::uint64_t playtimeMs = 0) const;
    [[nodiscard]] Result<PreparedRestore> PrepareRestore(
        const GameState& state, std::uint64_t nowMs = 0);
    Status CommitRestore(PreparedRestore prepared);
    Status RestoreState(const GameState& state, std::uint64_t nowMs = 0);

    [[nodiscard]] vn::VM& VM() { return m_vm; }
    [[nodiscard]] const vn::VM& VM() const { return m_vm; }
    [[nodiscard]] vn::Stage& Stage() { return m_stage; }
    [[nodiscard]] const vn::Stage& Stage() const { return m_stage; }
    [[nodiscard]] vn::Dialogue& Dialogue() { return m_dialogue; }
    [[nodiscard]] const vn::Dialogue& Dialogue() const { return m_dialogue; }
    [[nodiscard]] vn::VariableStore& Variables() { return m_variables; }
    [[nodiscard]] const vn::VariableStore& Variables() const { return m_variables; }
    [[nodiscard]] vn::Backlog& Backlog() { return m_backlog; }
    [[nodiscard]] const vn::Backlog& Backlog() const { return m_backlog; }
    [[nodiscard]] audio::AudioEngine& Audio() { return m_services.audio; }
    [[nodiscard]] const audio::AudioEngine& Audio() const { return m_services.audio; }
    [[nodiscard]] graphics::AssetCache& Assets() { return m_services.assets; }
    [[nodiscard]] const graphics::AssetCache& Assets() const {
        return m_services.assets;
    }
    [[nodiscard]] ui::UIRouter& Routes() { return m_routes; }
    [[nodiscard]] const ui::UIRouter& Routes() const { return m_routes; }
    [[nodiscard]] animation::TimelinePlayer& Timeline() { return m_timeline; }
    [[nodiscard]] const animation::TimelinePlayer& Timeline() const { return m_timeline; }
    [[nodiscard]] Result<animation::PlaybackHandle> PlayAnimationAsset(
        const std::string& path,bool await=false,float speed=1.0f);
    [[nodiscard]] Result<animation::PlaybackHandle> PlayTimelineAsset(
        const std::string& path,bool await=false,float speed=1.0f);
    [[nodiscard]] Result<animation::PlaybackHandle> PlayTimelineText(
        std::string_view text, const std::string& sourcePath,
        bool await=false, float speed=1.0f);
    using AnimationTargetValidator = std::function<Status(
        const animation::TrackBinding&, const Variant&,
        const ui::UIRuntimeState&)>;
    void SetAnimationTargetHandler(
        animation::TargetKind kind, animation::TimelinePlayer::Apply handler,
        AnimationTargetValidator validator = {}) {
        m_animationHandlers[kind] = std::move(handler);
        if (validator) m_animationValidators[kind] = std::move(validator);
        else m_animationValidators.erase(kind);
    }
    // RuntimeSession owns built-in command execution. Player and Preview may
    // inject presentation and extension behaviour without replacing it.
    void SetExtensionCommandHandler(std::function<bool(const vn::Command&)> handler) {
        m_extensionCommandHandler = std::move(handler);
    }
    void SetRoutePresentationHandler(
        std::function<void(std::string_view route, std::string_view operation)> handler) {
        m_routes.SetPresentationHandler(std::move(handler));
    }
    void SetUIStateHandler(
        std::function<ui::UIRuntimeState()> capture,
        std::function<Status(const ui::UIRuntimeState&)> restore,
        std::function<Status(const ui::UIRuntimeState&)> validate = {}) {
        m_captureUIState = std::move(capture);
        m_restoreUIState = std::move(restore);
        m_validateUIState = std::move(validate);
    }

private:
    Services m_services;
    vn::Dialogue m_dialogue;
    vn::VariableStore m_variables;
    vn::Backlog m_backlog;
    vn::Stage m_stage;
    vn::VM m_vm;
    ui::UIRouter m_routes;
    animation::TimelinePlayer m_timeline;
    std::string m_preloadScript;
    int m_preloadPc=-1;
    std::unordered_map<animation::TargetKind,animation::TimelinePlayer::Apply> m_animationHandlers;
    std::unordered_map<animation::TargetKind,AnimationTargetValidator> m_animationValidators;
    std::function<bool(const vn::Command&)> m_extensionCommandHandler;
    std::function<ui::UIRuntimeState()> m_captureUIState;
    std::function<Status(const ui::UIRuntimeState&)> m_restoreUIState;
    std::function<Status(const ui::UIRuntimeState&)> m_validateUIState;
    std::optional<animation::PlaybackHandle> m_awaitingTimeline;
    bool m_externalResumePending = false;
    std::vector<diag::Diagnostic> m_lastStartDiagnostics;
    std::shared_ptr<const vn::Program> m_runtimeProgramIdentity;
    std::optional<sdk::SourceMapDocument> m_sourceMap;
    std::string m_sourceMapPath;

    bool ExecuteCommand(const vn::Command& command);
    Status ApplyState(const GameState& state, std::uint64_t nowMs);
    std::optional<vn::Program> CompileRuntimeIrText(
        std::string_view text, const std::string& sourcePath);
    Result<resource::ResourceId> LoadAnimationAsset(
        const std::string& path,
        std::optional<resource::ResourceId> expectedId,
        std::vector<std::string>& stack);
};

}  // namespace px
