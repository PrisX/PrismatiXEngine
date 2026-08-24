#include "Engine/UI/UIContext.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Actions/BuiltInActionProvider.h"

#include <algorithm>
#include <limits>

namespace px::ui {

UIContext::UIContext() {
    auto provider=std::make_shared<BuiltInActionProvider>();
    (void)provider->Register("animation.trigger",[this](const ActionInvocation& invocation){const auto found=invocation.arguments.find("parameter");const auto* name=found==invocation.arguments.end()?nullptr:found->second.TryGet<std::string>();return name?SetAnimationTrigger(*name):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2407",.category="UI.Animation",.message="animation.trigger requires parameter"});});
    (void)provider->Register("animation.bool",[this](const ActionInvocation& invocation){const auto nameIt=invocation.arguments.find("parameter"),valueIt=invocation.arguments.find("value");const auto* name=nameIt==invocation.arguments.end()?nullptr:nameIt->second.TryGet<std::string>();const auto* value=valueIt==invocation.arguments.end()?nullptr:valueIt->second.TryGet<bool>();return name&&value?SetAnimationBool(*name,*value):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2408",.category="UI.Animation",.message="animation.bool requires parameter and value"});});
    (void)provider->Register("animation.number",[this](const ActionInvocation& invocation){const auto nameIt=invocation.arguments.find("parameter"),valueIt=invocation.arguments.find("value");const auto* name=nameIt==invocation.arguments.end()?nullptr:nameIt->second.TryGet<std::string>();double value=0;bool valid=false;if(valueIt!=invocation.arguments.end()){if(const auto* number=valueIt->second.TryGet<double>()){value=*number;valid=true;}else if(const auto* integer=valueIt->second.TryGet<std::int64_t>()){value=static_cast<double>(*integer);valid=true;}}return name&&valid?SetAnimationNumber(*name,value):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2409",.category="UI.Animation",.message="animation.number requires parameter and numeric value"});});
    (void)provider->Register("animation.travel",[this](const ActionInvocation& invocation){const auto stateIt=invocation.arguments.find("state"),durationIt=invocation.arguments.find("duration");const auto* state=stateIt==invocation.arguments.end()?nullptr:stateIt->second.TryGet<std::string>();double duration=0;bool valid=false;if(durationIt!=invocation.arguments.end()){if(const auto* number=durationIt->second.TryGet<double>()){duration=*number;valid=true;}else if(const auto* integer=durationIt->second.TryGet<std::int64_t>()){duration=static_cast<double>(*integer);valid=true;}}return state&&valid?TravelAnimationState(*state,static_cast<float>(std::max(0.0,duration))):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2410",.category="UI.Animation",.message="animation.travel requires state and duration"});});
    (void)provider->Register("visualState.set",[this](const ActionInvocation& invocation){const auto groupIt=invocation.arguments.find("group"),stateIt=invocation.arguments.find("state");const auto* group=groupIt==invocation.arguments.end()?nullptr:groupIt->second.TryGet<std::string>();const auto* state=stateIt==invocation.arguments.end()?nullptr:stateIt->second.TryGet<std::string>();return group&&state?SetVisualState(*group,*state):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2412",.category="UI.VisualState",.message="visualState.set requires group and state"});});
    provider->SetFallback([this](const ActionInvocation& invocation){
        return m_commands.Execute(invocation.action,Variant(invocation.arguments));
    });
    (void)m_actions.RegisterProvider(std::move(provider));
}

Status UIContext::SetRoot(std::unique_ptr<Control> root) {
    if (!root) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Fatal, .code = "PXUI2401",
                                    .category = "UI.Runtime", .message = "UI root cannot be null"};
        diag::Emit(diagnostic); return Status::Fail(std::move(diagnostic));
    }
    m_behaviors.CancelAll();m_actions.CancelSource(m_triggerSourceScene);m_triggerSourceScene.clear();m_triggerBindings.clear();m_bindings.clear();m_root = std::move(root); m_input = std::make_unique<InputRouter>(*m_root);
    m_animationController=std::make_unique<UIAnimationController>(*m_root);m_animationController->SetExternalClipResolver(m_animationResolver);
    m_visualStateController=std::make_unique<VisualStateController>(*m_root);
    m_visualStateController->SetClipResolver([this](const Uuid& id){return m_animationController&&m_animationController->Library()?m_animationController->Library()->FindClip(id):nullptr;});
    ConfigureControlRuntime();
    m_behaviors.SetServices(BehaviorServices());
    m_width = m_height = 0; return Status::Ok();
}

void UIContext::SetControlRuntimeConfigurator(std::function<void(Control&)> configure){m_configureControlRuntime=std::move(configure);ConfigureControlRuntime();}
void UIContext::ConfigureControlRuntime(){if(!m_root||!m_configureControlRuntime)return;std::function<void(scene::Node&)> visit=[&](scene::Node& node){if(auto* control=dynamic_cast<Control*>(&node))m_configureControlRuntime(*control);for(const auto& child:node.Children())visit(*child);};visit(*m_root);}

BehaviorRuntimeServices UIContext::BehaviorServices(){
    constexpr std::uint64_t embeddedAnimation=std::numeric_limits<std::uint64_t>::max();
    BehaviorRuntimeServices services;
    services.root=m_root.get();services.actions=&m_actions;
    services.readVariable=m_readBehaviorVariable;services.writeVariable=m_writeBehaviorVariable;
    services.playAnimation=[this](std::string_view name)->Result<std::uint64_t>{
        if(m_animationController&&m_animationController->Library()){
            const auto& library=*m_animationController->Library();
            if(name.empty()||name=="default"||name=="embedded"||library.machine.FindState(name)){
                const Status local=PlayAnimation(name);
                return local?Result<std::uint64_t>::Success(embeddedAnimation):Result<std::uint64_t>::Failure(local.Diagnostics());
            }
        }
        if(m_playExternalAnimation)return m_playExternalAnimation(name);
        return Result<std::uint64_t>::Failure({diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2402",.category="UI.Animation",.message="Animation state or external animation does not exist: "+std::string(name)}});
    };
    services.animationPlaying=[this,embeddedAnimation](std::uint64_t handle){if(handle==embeddedAnimation)return m_animationController&&m_animationController->ActivePlayback();return m_externalAnimationPlaying&&m_externalAnimationPlaying(handle);};
    services.setAnimationParameter=[this](std::string_view name,const Variant& value){return m_animationController?m_animationController->SetParameter(name,value):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});};
    services.travelAnimationState=[this](std::string_view state,float duration){return TravelAnimationState(state,duration);};
    return services;
}

void UIContext::SetBehaviorVariableAccess(
    std::function<std::optional<Variant>(std::string_view)> read,
    std::function<Status(std::string_view,const Variant&)> write){
    m_readBehaviorVariable=std::move(read);m_writeBehaviorVariable=std::move(write);
    m_behaviors.SetServices(BehaviorServices());
}

BehaviorRuntimeState UIContext::CaptureBehaviorState() const {
    return {.fibers=m_behaviors.CaptureState(), .actions=m_actions.CaptureState()};
}

Status UIContext::RestoreBehaviorState(const BehaviorRuntimeState& state) {
    const Status actions = m_actions.RestoreState(state.actions);
    if (!actions) return actions;
    return m_behaviors.RestoreState(state.fibers);
}

Status UIContext::ConfigureTriggers(std::vector<TriggerBinding> triggers,
                                    std::optional<BehaviorGraph> interactionGraph,
                                    std::string sourceScene){
    if(!m_root)return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
        .code="PXUI2403",.category="UI.Runtime",.message="ConfigureTriggers requires a UI root"});
    m_behaviors.CancelAll();m_actions.CancelSource(m_triggerSourceScene);m_triggerSourceScene.clear();m_behaviors.SetServices(BehaviorServices());
    if(interactionGraph){const Status status=m_behaviors.SetGraph(std::move(*interactionGraph),sourceScene);if(!status)return status;}
    for(auto& binding:triggers){
        auto* object=m_root->Find(binding.node);auto* control=dynamic_cast<Control*>(object);
        if(!control)return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
            .code="PXUI2404",.category="UI.Runtime",.message="Trigger target Control is missing",
            .details=binding.node.ToString()});
        if(!TypeRegistry::Global().FindSignal(std::string(control->TypeName()),binding.signal))
            return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
                .code="PXUI2411",.category="UI.Runtime",
                .message="Trigger signal does not match the Runtime TypeRegistry",
                .details=std::string(control->TypeName())+"."+binding.signal});
        binding.sourceScene=sourceScene;
        if(binding.kind==TriggerBindingKind::Action){
            (void)control->ConnectSignal(binding.signal,[this,binding](const Control::SignalArguments& signalArguments){
                ActionInvocation invocation=binding.Invocation();
                if(const auto* descriptor=m_actions.Catalog().Find(invocation.action))
                    for(const auto& argument:descriptor->arguments)if(!invocation.arguments.contains(argument.name)){
                        const auto found=signalArguments.find(argument.name);
                        if(found!=signalArguments.end())invocation.arguments[argument.name]=found->second.Clone();
                    }
                const Status status=m_actions.Dispatch(std::move(invocation),
                    {.reentryPolicy=binding.reentry});
                if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);
            });
        }else{
            (void)control->ConnectSignal(binding.signal,[this,binding](const Control::SignalArguments& arguments){
                const auto started=m_behaviors.Start(binding.graphEntry,arguments,
                    {.sourceScene=binding.sourceScene,.sourceNode=binding.node,
                     .signal=binding.signal},binding.reentry);
                if(!started)for(const auto& diagnostic:started.Diagnostics())diag::Emit(diagnostic);
            });
        }
    }
    m_triggerBindings=std::move(triggers);m_triggerSourceScene=std::move(sourceScene);return Status::Ok();
}

bool UIContext::Update(const Input& input, int viewportWidth, int viewportHeight,float deltaSeconds) {
    if (!m_root) return false;
    m_root->Update(deltaSeconds);
    if (m_width != viewportWidth || m_height != viewportHeight || m_root->LayoutDirty()) {
        m_width = viewportWidth; m_height = viewportHeight;
        (void)m_root->Measure({static_cast<float>(m_width), static_cast<float>(m_height)});
        m_root->Arrange({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)});
    }
    if (m_input) m_input->Update(input);
    if(m_animationController){const Status status=m_animationController->Update(deltaSeconds);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}
    if(m_visualStateController){const Status status=m_visualStateController->Update(deltaSeconds);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}
    m_actions.Update(deltaSeconds);
    m_behaviors.Update(deltaSeconds);
    if (m_root->LayoutDirty()) {
        (void)m_root->Measure({static_cast<float>(m_width), static_cast<float>(m_height)});
        m_root->Arrange({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)});
    }
    return m_input && m_input->LastFrameConsumed();
}

Status UIContext::SetAnimations(UIAnimationLibrary library,bool autoplay){if(!m_animationController)return Status::Ok();return m_animationController->SetLibrary(std::move(library),autoplay);}
Status UIContext::SetVisualStateGroups(std::vector<VisualStateGroup> groups){return m_visualStateController?m_visualStateController->SetGroups(std::move(groups)):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2413",.category="UI.VisualState",.message="Visual State Controller is not configured"});}
Status UIContext::SetVisualState(const std::string_view group,const std::string_view state){return m_visualStateController?m_visualStateController->SetState(group,state):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2413",.category="UI.VisualState",.message="Visual State Controller is not configured"});}
std::optional<std::string_view> UIContext::ActiveVisualState(const std::string_view group)const{return m_visualStateController?m_visualStateController->ActiveState(group):std::nullopt;}
VisualStateRuntimeState UIContext::CaptureVisualState()const{return m_visualStateController?m_visualStateController->CaptureState():VisualStateRuntimeState{};}
Status UIContext::RestoreVisualState(const VisualStateRuntimeState& state){return m_visualStateController?m_visualStateController->RestoreState(state):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2413",.category="UI.VisualState",.message="Visual State Controller is not configured"});}
Status UIContext::PlayAnimation(const std::string_view state){if(!m_animationController||!m_animationController->Library()){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXUI2402",.category="UI.Animation",.message="UI scene has no Animation State Machine"};diag::Emit(d);return Status::Fail(std::move(d));}if(state.empty()||state=="default"||state=="embedded")return m_animationController->Travel(m_animationController->Library()->machine.entry);return m_animationController->Travel(state);}
Status UIContext::SetAnimationTrigger(const std::string_view parameter){return m_animationController?m_animationController->SetTrigger(parameter):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});}
Status UIContext::SetAnimationBool(const std::string_view parameter,const bool value){return m_animationController?m_animationController->SetBool(parameter,value):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});}
Status UIContext::SetAnimationNumber(const std::string_view parameter,const double value){return m_animationController?m_animationController->SetNumber(parameter,value):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});}
Status UIContext::SetAnimationParameter(const std::string_view parameter,const Variant& value){return m_animationController?m_animationController->SetParameter(parameter,value):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});}
Status UIContext::TravelAnimationState(const std::string_view state,const float duration){return m_animationController?m_animationController->Travel(state,duration):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});}
Status UIContext::PreviewAnimation(const Uuid& clip,float time,bool playing){
    if(!m_animationController||!m_animationController->Library()||m_animationController->Library()->clips.empty()){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXUI2402",.category="UI.Animation",.message="UI scene has no animation Clip to preview"};diag::Emit(d);return Status::Fail(std::move(d));}
    return m_animationController->PreviewClip(clip.Empty()?m_animationController->Library()->clips.front().id:clip,time,playing);
}
Status UIContext::StopAnimation(bool restoreDesignState){return m_animationController?m_animationController->Stop(restoreDesignState):Status::Ok();}
UIAnimationRuntimeState UIContext::CaptureAnimationState()const{return m_animationController?m_animationController->CaptureState():UIAnimationRuntimeState{};}
Status UIContext::RestoreAnimationState(const UIAnimationRuntimeState& state){return m_animationController?m_animationController->RestoreState(state):Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXUI2406",.category="UI.Animation",.message="Animation Controller is not configured"});}

void UIContext::Render(graphics::Renderer2D& renderer) {
    if (m_root) m_root->Draw(renderer, m_theme);
    if (m_showDiagnostics) RenderDiagnosticOverlay(renderer);
}

void UIContext::RenderDiagnosticOverlay(graphics::Renderer2D& renderer) {
    auto diagnostics = diag::Global().Snapshot();
    std::erase_if(diagnostics, [](const diag::Diagnostic& item) { return item.severity < diag::Severity::Error; });
    if (diagnostics.empty()) return;
    constexpr float width = 560.0f, lineHeight = 30.0f, padding = 16.0f;
    const std::size_t count = std::min<std::size_t>(5, diagnostics.size());
    const float height = padding * 2.0f + lineHeight * static_cast<float>(count + 1);
    const Rect area{std::max(0.0f, static_cast<float>(m_width) - width - 18.0f), 18.0f, width, height};
    renderer.DrawRoundedRect(area, 8.0f, {79, 25, 32, 245});
    renderer.DrawText("PrismatiX diagnostics", area.x + padding, area.y + padding,
                      "Content/Fonts/NotoSansTC-Bold.ttf", 22, {255, 224, 228, 255});
    for (std::size_t i = 0; i < count; ++i) {
        std::string text = diagnostics[diagnostics.size() - count + i].code + "  " + diagnostics[diagnostics.size() - count + i].message;
        if (text.size() > 76) text.resize(73), text += "...";
        renderer.DrawText(text, area.x + padding, area.y + padding + lineHeight * static_cast<float>(i + 1),
                          "Content/Fonts/NotoSansTC-Bold.ttf", 18, {255, 238, 240, 255});
    }
}

}  // namespace px::ui
