#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <set>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
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
        vn::DialogueSnapshot dialogue;
        std::unordered_map<std::string, int> variables;
        std::unordered_map<std::string, vn::Value> typedVariables;
        std::set<std::string> persistentVariables;
        vn::Stage::RuntimeState stage;
        audio::AudioEngine::RuntimeState audio;
        std::vector<vn::BacklogEntry> backlog;
        ui::RouteState routes;
        std::vector<animation::PlaybackState> timelines;
        std::vector<animation::AnimationClip> animationClips;
        ui::BehaviorRuntimeState behavior;
        std::uint64_t playtimeMs = 0;
    };

    explicit RuntimeSession(Services services);

    bool StartScenario(const std::string& scriptPath, bool resetVariables = true);
    bool StartScenarioText(std::string_view text, const std::string& scriptPath,
                           bool resetVariables = true);
    bool StartRuntimeIr(const std::string& sourcePath,
                        bool resetVariables = true);
    bool StartRuntimeIrText(std::string_view text, const std::string& sourcePath,
                            bool resetVariables = true);
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
    [[nodiscard]] ui::UIRouter& Routes() { return m_routes; }
    [[nodiscard]] const ui::UIRouter& Routes() const { return m_routes; }
    [[nodiscard]] animation::TimelinePlayer& Timeline() { return m_timeline; }
    [[nodiscard]] const animation::TimelinePlayer& Timeline() const { return m_timeline; }
    [[nodiscard]] Result<animation::PlaybackHandle> PlayAnimationAsset(
        const std::string& path,bool await=false,float speed=1.0f);
    void SetAnimationTargetHandler(animation::TargetKind kind,animation::TimelinePlayer::Apply handler){m_animationHandlers[kind]=std::move(handler);}
    // RuntimeSession owns built-in command execution. Player and Preview may
    // inject presentation and extension behaviour without replacing it.
    void SetExtensionCommandHandler(std::function<bool(const vn::Command&)> handler) {
        m_extensionCommandHandler = std::move(handler);
    }
    void SetRoutePresentationHandler(
        std::function<void(std::string_view route, std::string_view operation)> handler) {
        m_routePresentationHandler = std::move(handler);
    }
    void SetBehaviorStateHandler(
        std::function<ui::BehaviorRuntimeState()> capture,
        std::function<Status(const ui::BehaviorRuntimeState&)> restore) {
        m_captureBehaviorState = std::move(capture);
        m_restoreBehaviorState = std::move(restore);
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
    std::function<bool(const vn::Command&)> m_extensionCommandHandler;
    std::function<void(std::string_view, std::string_view)> m_routePresentationHandler;
    std::function<ui::BehaviorRuntimeState()> m_captureBehaviorState;
    std::function<Status(const ui::BehaviorRuntimeState&)> m_restoreBehaviorState;
    std::optional<animation::PlaybackHandle> m_awaitingTimeline;
    bool m_externalResumePending = false;
    std::vector<diag::Diagnostic> m_lastStartDiagnostics;

    bool ExecuteCommand(const vn::Command& command);
    std::optional<vn::Program> CompileRuntimeIrText(
        std::string_view text, const std::string& sourcePath);
};

}  // namespace px
