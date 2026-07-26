#include "Engine/Session/RuntimeSession.h"
#include "Engine/Session/RuntimeIrAdapter.h"
#include "Engine/SDK/RuntimeIr.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace px {
namespace {

Status RestoreFailure(std::string message, std::string details = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = "PXRUNTIME7301",
                                .category = "Runtime.Session",
                                .message = std::move(message),
                                .details = std::move(details)};
    diag::Emit(diagnostic);
    return Status::Fail(std::move(diagnostic));
}

bool CommandBool(const vn::Command& command, std::string_view name, bool fallback = false) {
    if (const Variant* value = command.FindTyped(name)) {
        if (const auto* boolean = value->TryGet<bool>()) return *boolean;
    }
    const std::string text = command.Get(name);
    if (text.empty()) return fallback;
    return text == "true" || text == "1" || text == "yes";
}

double CommandNumber(const vn::Command& command, std::string_view name, double fallback) {
    if (const Variant* value = command.FindTyped(name)) {
        if (const auto* number = value->TryGet<double>()) return *number;
        if (const auto* integer = value->TryGet<std::int64_t>()) return static_cast<double>(*integer);
    }
    try { return std::stod(command.Get(name)); } catch (...) { return fallback; }
}

std::string ResourcePath(const vn::Command& command, std::string_view name) {
    if (const Variant* value = command.FindTyped(name)) {
        if (const auto* reference = value->TryGet<ResourceRefValue>()) return reference->lastKnownPath;
    }
    return command.Get(name);
}

}  // namespace

RuntimeSession::RuntimeSession(Services services)
    : m_services(services),
      m_stage(services.renderer, services.assets),
      m_vm(services.vfs, services.audio, m_stage, m_dialogue, m_variables, m_backlog) {
    for (auto& preset : animation::OfficialPresets()) (void)m_timeline.Register(std::move(preset));
    m_timeline.SetApply([this](const animation::TrackBinding& binding,
                              const Variant& value) -> Status {
        if (binding.kind == animation::TargetKind::Stage ||
            binding.kind == animation::TargetKind::Camera) {
            const std::string target = binding.kind == animation::TargetKind::Camera
                                           ? "$camera"
                                           : binding.target;
            if (m_stage.ApplyAnimationProperty(target, binding.property, value)) {
                return Status::Ok();
            }
            diag::Diagnostic diagnostic{.severity=diag::Severity::Error,
                                        .code="PXRUNTIME7302",
                                        .category="Runtime.Animation",
                                        .message="Animation track cannot bind to stage property",
                                        .details=target+"."+binding.property};
            return Status::Fail(std::move(diagnostic));
        }
        if(binding.kind==animation::TargetKind::Audio){const auto number=[&]()->std::optional<double>{if(const auto* valueNumber=value.TryGet<double>())return *valueNumber;if(const auto* integer=value.TryGet<std::int64_t>())return static_cast<double>(*integer);return std::nullopt;}();if(!number)return RestoreFailure("Audio animation value must be numeric",binding.property);const int volume=static_cast<int>(std::clamp(*number<=1.0?*number*128.0:*number,0.0,128.0));if(binding.target=="main")m_services.audio.SetMainVolume(volume);else if(binding.target=="music")m_services.audio.SetBGMVolume(volume);else if(binding.target=="voice")m_services.audio.SetVoiceVolume(volume);else if(binding.target=="sfx")m_services.audio.SetSEVolume(volume);else if(binding.target=="ambience")m_services.audio.SetAmbienceVolume(volume);else return RestoreFailure("Unknown audio animation bus",binding.target);return Status::Ok();}
        if(binding.kind==animation::TargetKind::Shader&&m_stage.ApplyAnimationProperty("$camera",binding.property,value))return Status::Ok();
        if(const auto found=m_animationHandlers.find(binding.kind);found!=m_animationHandlers.end())return found->second(binding,value);
        return RestoreFailure("Animation target has no runtime binding",binding.target+"."+binding.property);
    });
    m_timeline.SetCompletion([this](const animation::PlaybackHandle handle, bool) {
        if (m_awaitingTimeline && *m_awaitingTimeline == handle) {
            m_awaitingTimeline.reset();
            m_externalResumePending = true;
        }
    });
    m_vm.SetCommandHook([this](const vn::Command& command) { return ExecuteCommand(command); });
}

Result<animation::PlaybackHandle> RuntimeSession::PlayAnimationAsset(
    const std::string& path,const bool await,const float speed){
    const auto source=m_services.vfs.ReadText(path);if(!source)return Result<animation::PlaybackHandle>::Failure(RestoreFailure("Animation clip could not be read",path).Diagnostics());
    auto parsed=animation::ParseAnimationClip(*source,path);if(!parsed)return Result<animation::PlaybackHandle>::Failure(parsed.Diagnostics());const auto id=parsed.Value().id;
    if(!m_timeline.Find(id)){const Status registered=m_timeline.Register(parsed.TakeValue());if(!registered)return Result<animation::PlaybackHandle>::Failure(registered.Diagnostics());}
    const auto handle=m_timeline.Play(id,await,speed);if(!handle)return Result<animation::PlaybackHandle>::Failure(RestoreFailure("Animation clip could not be played",path).Diagnostics());return Result<animation::PlaybackHandle>::Success(handle);
}

bool RuntimeSession::ExecuteCommand(const vn::Command& command) {
    if (command.type == "ambience") {
        const std::string path = ResourcePath(command, "file");
        if (!path.empty()) {
            m_services.audio.PlayAmbience(path, true,
                static_cast<int>(CommandNumber(command, "fade", 0.0)));
        }
        return true;
    }
    if (command.type == "stopambience") {
        m_services.audio.StopAmbience(static_cast<int>(CommandNumber(command, "fade", 0.0)));
        return true;
    }
    if (command.type == "route") {
        const std::string route = command.Get("route");
        const std::string operation = command.Get("operation", "replace");
        Status status;
        if (operation == "push") status = m_routes.Push(route);
        else if (operation == "modal") status = m_routes.ShowModal(route);
        else if (operation == "back") status = m_routes.Back();
        else status = m_routes.Replace(route);
        if (!status) for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
        if (status && m_routePresentationHandler) m_routePresentationHandler(route, operation);
        return true;
    }
    if (command.type == "animation") {
        const std::string path = ResourcePath(command, "clip");
        const bool await = CommandBool(command, "await");
        auto played=PlayAnimationAsset(path,await,static_cast<float>(CommandNumber(command,"speed",1.0)));
        if(!played){for(const auto& diagnostic:played.Diagnostics())diag::Emit(diagnostic);}
        else if (await) {
            m_awaitingTimeline = played.Value();
            m_vm.WaitExternal();
        }
        return true;
    }
    if (command.type == "screen_effect") {
        const std::string preset = command.Get("preset");
        const animation::AnimationClip* selected = nullptr;
        for (const auto& [_, clip] : m_timeline.RegisteredClips()) {
            if (clip.name == preset || clip.name == "Screen/" + preset) { selected = &clip; break; }
        }
        if (!selected) {
            (void)RestoreFailure("Unknown screen effect preset", preset);
            return true;
        }
        float speed = 1.0f;
        const double durationMs = CommandNumber(command, "duration", 0.0);
        if (durationMs > 0.0 && selected->duration > 0.0f)
            speed = selected->duration / static_cast<float>(durationMs / 1000.0);
        const bool await = CommandBool(command, "await");
        const auto handle = m_timeline.Play(selected->id, await, speed);
        if (await && handle) { m_awaitingTimeline = handle; m_vm.WaitExternal(); }
        return true;
    }
    return m_extensionCommandHandler ? m_extensionCommandHandler(command) : false;
}

bool RuntimeSession::StartScenario(const std::string& scriptPath, const bool resetVariables) {
    m_stage.ClearAll();
    m_dialogue.Clear();
    m_backlog.Clear();
    if (resetVariables) m_variables.Reset(false);
    return m_vm.LoadScript(scriptPath);
}

bool RuntimeSession::StartScenarioText(const std::string_view text,
                                       const std::string& scriptPath,
                                       const bool resetVariables) {
    m_stage.ClearAll();
    m_dialogue.Clear();
    m_backlog.Clear();
    if (resetVariables) m_variables.Reset(false);
    return m_vm.LoadScenarioText(text, scriptPath);
}

bool RuntimeSession::StartRuntimeIrText(const std::string_view text,
                                        const std::string& sourcePath,
                                        const bool resetVariables) {
    const sdk::RuntimeIrParseResult parsed = sdk::ParseRuntimeIr(text);
    if (!parsed.Valid()) return false;
    m_stage.ClearAll();
    m_dialogue.Clear();
    m_backlog.Clear();
    if (resetVariables) m_variables.Reset(false);
    return m_vm.LoadCompiledProgram(CompileRuntimeIr(parsed.document), sourcePath);
}

void RuntimeSession::Update(const std::uint64_t nowMs, const float deltaSeconds) {
    m_vm.Update(nowMs, deltaSeconds);
    m_timeline.Update(deltaSeconds);
    if (m_externalResumePending) {
        m_externalResumePending = false;
        m_vm.NotifyExternalDone();
    }
    if(m_preloadScript!=m_vm.CurrentScript()||m_preloadPc!=m_vm.ProgramCounter()){
        m_preloadScript=m_vm.CurrentScript();m_preloadPc=m_vm.ProgramCounter();const auto& program=m_vm.CurrentProgram();const int end=std::min(static_cast<int>(program.code.size()),m_preloadPc+16);
        for(int index=std::max(0,m_preloadPc);index<end;++index){
            const auto& command=program.code[static_cast<std::size_t>(index)];
            if(command.type!="bg"&&command.type!="char"&&command.type!="cg"&&command.type!="layer")continue;
            for(const char* field:{"file","image","rule"}){
                const Variant* value=command.FindTyped(field);const auto* resource=value?value->TryGet<ResourceRefValue>():nullptr;
                if(resource&&!resource->lastKnownPath.empty())m_services.assets.PreloadTexture(resource->lastKnownPath);
            }
        }
    }
}

void RuntimeSession::Advance() {
    m_vm.OnAdvance();
}

void RuntimeSession::SelectChoice(const int index) {
    m_vm.SelectChoice(index);
}

RuntimeSession::GameState RuntimeSession::CaptureState(const std::uint64_t playtimeMs) const {
    GameState state;
    state.vm = m_vm.CaptureState();
    state.dialogue = m_dialogue.CaptureState();
    state.variables = m_variables.All();
    for (const auto& [name, entry] : m_variables.Values()) {
        if (entry.scope != vn::VariableScope::Temporary) {
            state.typedVariables.emplace(name, entry.value.Clone());
        }
    }
    state.persistentVariables = m_variables.PersistentKeys();
    state.stage = m_stage.CaptureState();
    state.audio = m_services.audio.CaptureState();
    state.backlog = m_backlog.Entries();
    state.routes = m_routes.CaptureState();
    state.timelines = m_timeline.CaptureState();
    for (const auto& [_, clip] : m_timeline.RegisteredClips()) state.animationClips.push_back(clip);
    if (m_captureBehaviorState) state.behavior = m_captureBehaviorState();
    state.playtimeMs = playtimeMs;
    return state;
}

Status RuntimeSession::RestoreState(const GameState& state, const std::uint64_t nowMs) {
    // Construct all fallible route state before mutating gameplay state.
    if ((!state.routes.stack.empty() || !state.routes.modals.empty())) {
        const Status routeStatus = m_routes.RestoreState(state.routes);
        if (!routeStatus) return routeStatus;
    }
    for (const auto& clip : state.animationClips)
        if (!m_timeline.Find(clip.id)) {
            const Status registered = m_timeline.Register(clip);
            if (!registered) return registered;
        }
    if (const Status timelineStatus = m_timeline.RestoreState(state.timelines);
        !timelineStatus) {
        return timelineStatus;
    }
    m_awaitingTimeline.reset();
    for (const auto& playback : state.timelines)
        if (playback.playing && playback.awaiting) { m_awaitingTimeline = playback.handle; break; }
    m_variables.Reset(false);
    for (const auto& [name, value] : state.typedVariables) {
        m_variables.SetValue(name, value.Clone(), state.persistentVariables.contains(name)
                                                   ? vn::VariableScope::Persistent
                                                   : vn::VariableScope::SaveLocal);
    }
    if (const Status stageStatus = m_stage.RestoreState(state.stage); !stageStatus) {
        return stageStatus;
    }
    if (!m_services.audio.RestoreState(state.audio)) {
        return RestoreFailure("Audio state could not be restored");
    }
    m_backlog.Clear();
    for (const auto& entry : state.backlog) {
        m_backlog.Push(entry.speaker, entry.text, entry.voice, entry.isChoice);
    }
    m_dialogue.RestoreState(state.dialogue);
    if (!m_vm.RestoreState(state.vm, nowMs)) {
        return RestoreFailure("Scenario execution state could not be restored",
                              state.vm.scriptPath);
    }
    m_vm.OverrideCurrentBgm(state.vm.currentBgm);
    if (m_restoreBehaviorState) {
        const Status behaviorStatus = m_restoreBehaviorState(state.behavior);
        if (!behaviorStatus) return behaviorStatus;
    }
    return Status::Ok();
}

}  // namespace px
