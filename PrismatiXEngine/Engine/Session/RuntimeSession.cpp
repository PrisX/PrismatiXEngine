#include "Engine/Session/RuntimeSession.h"
#include "Engine/Session/RuntimeAssetResolver.h"
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
    std::vector<std::string> stack;
    auto loaded=LoadAnimationAsset(path,std::nullopt,stack);
    if(!loaded)return Result<animation::PlaybackHandle>::Failure(loaded.Diagnostics());
    const auto handle=m_timeline.Play(loaded.Value(),await,speed);if(!handle)return Result<animation::PlaybackHandle>::Failure(RestoreFailure("Animation clip could not be played",path).Diagnostics());return Result<animation::PlaybackHandle>::Success(handle);
}

Result<animation::PlaybackHandle> RuntimeSession::PlayTimelineAsset(
    const std::string& path,const bool await,const float speed){
    if(!path.ends_with(".pxtimeline"))return Result<animation::PlaybackHandle>::Failure(RestoreFailure("Timeline asset must use the .pxtimeline extension",path).Diagnostics());
    return PlayAnimationAsset(path,await,speed);
}

Result<resource::ResourceId> RuntimeSession::LoadAnimationAsset(
    const std::string& path,
    const std::optional<resource::ResourceId> expectedId,
    std::vector<std::string>& stack){
    if(expectedId&&m_timeline.Find(*expectedId))
        return Result<resource::ResourceId>::Success(*expectedId);
    if(std::ranges::find(stack,path)!=stack.end())
        return Result<resource::ResourceId>::Failure(
            RestoreFailure("Nested animation resource cycle detected",path).Diagnostics());
    const auto source=m_services.vfs.ReadText(path);
    if(!source)return Result<resource::ResourceId>::Failure(
        RestoreFailure("Animation asset could not be read",path).Diagnostics());
    stack.push_back(path);
    const auto pop=[&]{stack.pop_back();};
    if(path.ends_with(".pxtimeline")){
        auto parsed=animation::ParseTimeline(*source,path);
        if(!parsed){pop();return Result<resource::ResourceId>::Failure(parsed.Diagnostics());}
        animation::TimelineDocument document=parsed.TakeValue();
        if(expectedId)document.clip.id=*expectedId;
        if(m_timeline.Find(document.clip.id)){const auto id=document.clip.id;pop();return Result<resource::ResourceId>::Success(id);}
        for(const auto& nested:document.nestedClips){
            resource::ResourceId nestedId=nested.clip.id;
            if(!nested.clip.lastKnownPath.empty()){
                auto loaded=LoadAnimationAsset(nested.clip.lastKnownPath,
                    nested.clip.id.Empty()?std::nullopt:std::optional<resource::ResourceId>{nested.clip.id},stack);
                if(!loaded){pop();return Result<resource::ResourceId>::Failure(loaded.Diagnostics());}
                nestedId=loaded.Value();
            }else if(nestedId.Empty()||!m_timeline.Find(nestedId)){
                pop();return Result<resource::ResourceId>::Failure(
                    RestoreFailure("Nested animation resource has no resolvable path or registered id",path).Diagnostics());
            }
            document.clip.nested.push_back({nested.start,nestedId,nested.speed});
        }
        const auto id=document.clip.id;
        const Status registered=m_timeline.Register(std::move(document.clip));
        pop();
        return registered?Result<resource::ResourceId>::Success(id)
                         :Result<resource::ResourceId>::Failure(registered.Diagnostics());
    }
    auto parsed=animation::ParseAnimationClip(*source,path);
    if(!parsed){pop();return Result<resource::ResourceId>::Failure(parsed.Diagnostics());}
    animation::AnimationClip clip=parsed.TakeValue();
    if(expectedId)clip.id=*expectedId;
    const auto id=clip.id;
    if(!m_timeline.Find(id)){
        const Status registered=m_timeline.Register(std::move(clip));
        if(!registered){pop();return Result<resource::ResourceId>::Failure(registered.Diagnostics());}
    }
    pop();return Result<resource::ResourceId>::Success(id);
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

bool RuntimeSession::StartRuntimeIr(const std::string& sourcePath,
                                    const bool resetVariables) {
    const auto source = m_services.vfs.ReadText(sourcePath);
    if (!source) {
        m_lastStartDiagnostics.clear();
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXRUNTIME7310",
            .category = "Runtime.IR",
            .message = "Compiled Story Runtime IR could not be read",
            .details = sourcePath};
        diagnostic.source.path = sourcePath;
        m_lastStartDiagnostics.push_back(std::move(diagnostic));
        diag::Emit(m_lastStartDiagnostics.front());
        return false;
    }
    return StartRuntimeIrText(*source, sourcePath, resetVariables);
}

bool RuntimeSession::StartRuntimeIrText(const std::string_view text,
                                        const std::string& sourcePath,
                                        const bool resetVariables) {
    auto program = CompileRuntimeIrText(text, sourcePath);
    if (!program) return false;
    m_stage.ClearAll();
    m_dialogue.Clear();
    m_backlog.Clear();
    if (resetVariables) m_variables.Reset(false);
    return m_vm.LoadCompiledProgram(std::move(*program), sourcePath);
}

vn::ProgramPatchStatus RuntimeSession::PatchRuntimeIrText(
    const std::string_view text, const std::string& sourcePath) {
    auto program = CompileRuntimeIrText(text, sourcePath);
    if (!program) return vn::ProgramPatchStatus::InvalidProgram;
    return m_vm.PatchCompiledProgram(std::move(*program), sourcePath);
}

vn::ProgramSeekStatus RuntimeSession::SeekRuntimeIrOperation(
    const std::string_view text, const std::string& sourcePath,
    const int operationIndex) {
    auto program = CompileRuntimeIrText(text, sourcePath);
    if (!program) return vn::ProgramSeekStatus::InvalidProgram;
    m_stage.ClearAll();
    m_dialogue.Clear();
    m_backlog.Clear();
    m_variables.Reset(false);
    return m_vm.LoadCompiledProgramAt(
        std::move(*program), sourcePath, operationIndex);
}

std::optional<vn::Program> RuntimeSession::CompileRuntimeIrText(
    const std::string_view text, const std::string& sourcePath) {
    m_lastStartDiagnostics.clear();
    const sdk::RuntimeIrParseResult parsed = sdk::ParseRuntimeIr(text);
    if (!parsed.Valid()) {
        for (const auto& issue : parsed.diagnostics) {
            diag::Diagnostic diagnostic{
                .severity = diag::Severity::Error,
                .code = issue.code,
                .category = "Runtime.IR",
                .message = issue.message};
            diagnostic.source.path = sourcePath;
            if (issue.operationIndex < parsed.document.operations.size()) {
                diagnostic.source.line = static_cast<int>(
                    parsed.document.operations[issue.operationIndex].sourceLine);
                diagnostic.operationId =
                    parsed.document.operations[issue.operationIndex].operationId;
            }
            m_lastStartDiagnostics.push_back(std::move(diagnostic));
        }
        for (const auto& diagnostic : m_lastStartDiagnostics) diag::Emit(diagnostic);
        return std::nullopt;
    }
    vn::Program program = CompileRuntimeIr(parsed.document);
    if (!program.errors.empty()) {
        for (const auto& error : program.errors) {
            diag::Diagnostic diagnostic{
                .severity = diag::Severity::Error,
                .code = "PXRUNTIME7309",
                .category = "Runtime.IR",
                .message = "Runtime IR could not be compiled",
                .details = error};
            diagnostic.source.path = sourcePath;
            m_lastStartDiagnostics.push_back(std::move(diagnostic));
        }
        for (const auto& diagnostic : m_lastStartDiagnostics) diag::Emit(diagnostic);
        return std::nullopt;
    }
    if (UsesRuntimeAssetReferences(program)) {
        const auto manifest = m_services.vfs.ReadText("project.pxproject");
        auto resolved = ResolveRuntimeAssetReferences(
            std::move(program), manifest.value_or(std::string{}),
            [this](const std::string_view path) {
                return m_services.vfs.Exists(path);
            },
            sourcePath);
        if (!resolved) {
            m_lastStartDiagnostics = resolved.Diagnostics();
            for (const auto& diagnostic : m_lastStartDiagnostics) {
                diag::Emit(diagnostic);
            }
            return std::nullopt;
        }
        program = resolved.TakeValue();
    }
    return program;
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
    state.runtimeProgram = m_vm.CurrentProgram();
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
    bool vmRestored = true;
    if (!state.vm.scriptPath.empty() || !state.runtimeProgram.code.empty()) {
        vmRestored = state.runtimeProgram.code.empty()
            ? m_vm.RestoreState(state.vm, nowMs)
            : m_vm.RestoreCompiledState(state.vm, state.runtimeProgram, nowMs);
    }
    if (!vmRestored) {
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
