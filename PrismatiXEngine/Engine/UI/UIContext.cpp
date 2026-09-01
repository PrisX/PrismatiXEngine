#include "Engine/UI/UIContext.h"

#include "Engine/Animation/Timeline.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/Text/Typography.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Actions/BuiltInActionProvider.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace px::ui {
namespace {

Control* FindControlByName(Control* root, const std::string_view name) {
    if (!root) return nullptr;
    if (root->Name() == name) return root;
    for (const auto& child : root->Children())
        if (auto* control = dynamic_cast<Control*>(child.get()))
            if (auto* found = FindControlByName(control, name)) return found;
    return nullptr;
}

Status AnimationPropertyFailure(std::string code, std::string message) {
    diag::Diagnostic diagnostic;
    diagnostic.severity = diag::Severity::Error;
    diagnostic.code = std::move(code);
    diagnostic.category = "UI.Animation";
    diagnostic.message = std::move(message);
    return Status::Fail(std::move(diagnostic));
}

}  // namespace

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
    UIDocumentInstallation installation;
    installation.root = std::move(root);
    installation.diagnosticOverlay = m_showDiagnostics;
    return InstallDocument(std::move(installation));
}

Status UIContext::ValidateTriggers(
    Control& root, const std::vector<TriggerBinding>& triggers,
    const BehaviorGraph* graph) const {
    for (const auto& binding : triggers) {
        auto* object = root.Find(binding.node);
        auto* control = dynamic_cast<Control*>(object);
        if (!control)
            return Status::Fail(diag::Diagnostic{
                .severity=diag::Severity::Error,.code="PXUI2404",
                .category="UI.Runtime",
                .message="Trigger target Control is missing",
                .details=binding.node.ToString()});
        if (!TypeRegistry::Global().FindSignal(
                std::string(control->TypeName()), binding.signal))
            return Status::Fail(diag::Diagnostic{
                .severity=diag::Severity::Error,.code="PXUI2411",
                .category="UI.Runtime",
                .message="Trigger signal does not match the Runtime TypeRegistry",
                .details=std::string(control->TypeName())+"."+binding.signal});
        if (binding.kind == TriggerBindingKind::Flow &&
            (!graph || !graph->Find(binding.graphEntry)))
            return Status::Fail(diag::Diagnostic{
                .severity=diag::Severity::Error,.code="PXUI2416",
                .category="UI.Runtime",
                .message="Flow trigger entry does not exist in the Behavior Graph",
                .details=binding.graphEntry.ToString()});
    }
    return Status::Ok();
}

std::vector<UIContext::TriggerConnection> UIContext::ConnectTriggers(
    Control& root, const std::vector<TriggerBinding>& triggers) {
    std::vector<TriggerConnection> connections;
    connections.reserve(triggers.size());
    for (const auto& binding : triggers) {
        auto* control = dynamic_cast<Control*>(root.Find(binding.node));
        if (binding.kind == TriggerBindingKind::Action) {
            const auto connection = control->ConnectSignal(
                binding.signal,
                [this,binding](const Control::SignalArguments& signalArguments) {
                    ActionInvocation invocation=binding.Invocation();
                    if(const auto* descriptor=m_actions.Catalog().Find(invocation.action))
                        for(const auto& argument:descriptor->arguments)
                            if(!invocation.arguments.contains(argument.name)){
                                const auto found=signalArguments.find(argument.name);
                                if(found!=signalArguments.end())
                                    invocation.arguments[argument.name]=found->second.Clone();
                            }
                    const Status status=m_actions.Dispatch(
                        std::move(invocation),{.reentryPolicy=binding.reentry});
                    if(!status)for(const auto& diagnostic:status.Diagnostics())
                        diag::Emit(diagnostic);
                });
            connections.push_back({binding.node,binding.signal,connection});
        } else {
            const auto connection = control->ConnectSignal(
                binding.signal,
                [this,binding](const Control::SignalArguments& arguments) {
                    const auto started=m_behaviors.Start(
                        binding.graphEntry,arguments,
                        {.sourceScene=binding.sourceScene,.sourceNode=binding.node,
                         .signal=binding.signal},binding.reentry);
                    if(!started)for(const auto& diagnostic:started.Diagnostics())
                        diag::Emit(diagnostic);
                });
            connections.push_back({binding.node,binding.signal,connection});
        }
    }
    return connections;
}

void UIContext::DisconnectTriggers(
    Control& root, const std::vector<TriggerConnection>& connections) {
    for (const auto& connection : connections)
        if (auto* control=dynamic_cast<Control*>(root.Find(connection.node)))
            (void)control->DisconnectSignal(connection.signal,
                                            connection.connection);
}

Status UIContext::InstallDocument(UIDocumentInstallation installation) {
    if (!installation.root) {
        diag::Diagnostic diagnostic{.severity=diag::Severity::Fatal,
            .code="PXUI2401",.category="UI.Runtime",
            .message="UI root cannot be null"};
        diag::Emit(diagnostic);
        return Status::Fail(std::move(diagnostic));
    }

    // Every fallible operation below targets the detached candidate tree.
    // Existing bindings, actions, focus, animations, and controls remain live
    // until this complete candidate has passed validation.
    auto candidateInput=std::make_unique<InputRouter>(*installation.root);
    auto candidateAnimations=std::make_unique<UIAnimationController>(
        *installation.root);
    candidateAnimations->SetExternalClipResolver(m_animationResolver);
    if(installation.animations){
        const Status status=candidateAnimations->SetLibrary(
            std::move(*installation.animations),installation.autoplayAnimations);
        if(!status)return status;
    }
    auto candidateVisualStates=std::make_unique<VisualStateController>(
        *installation.root);
    candidateVisualStates->SetClipResolver(
        [controller=candidateAnimations.get()](const Uuid& id){
            return controller&&controller->Library()
                ?controller->Library()->FindClip(id):nullptr;
        });
    if(!installation.visualStateGroups.empty()){
        const Status status=candidateVisualStates->SetGroups(
            std::move(installation.visualStateGroups));
        if(!status)return status;
    }
    BehaviorGraphRunner candidateBehaviors;
    if(installation.behaviorGraph){
        const Status status=candidateBehaviors.SetGraph(
            *installation.behaviorGraph,installation.sourceScene);
        if(!status)return status;
    }
    for(auto& trigger:installation.triggers)
        trigger.sourceScene=installation.sourceScene;
    if(const Status status=ValidateTriggers(
           *installation.root,installation.triggers,
           installation.behaviorGraph?&*installation.behaviorGraph:nullptr);
       !status)return status;
    auto candidateConnections=ConnectTriggers(
        *installation.root,installation.triggers);

    // Commit is deliberately no-fail. Disconnect observers that reference the
    // old tree before destroying it, then atomically publish the complete set
    // of candidate controllers at a frame boundary chosen by the caller.
    m_behaviors.CancelAll();
    m_actions.CancelSource(m_triggerSourceScene);
    m_bindings.clear();
    if(m_root)DisconnectTriggers(*m_root,m_triggerConnections);
    m_root=std::move(installation.root);
    m_input=std::move(candidateInput);
    m_animationController=std::move(candidateAnimations);
    m_visualStateController=std::move(candidateVisualStates);
    m_behaviors=std::move(candidateBehaviors);
    m_triggerBindings=std::move(installation.triggers);
    m_triggerConnections=std::move(candidateConnections);
    m_triggerSourceScene=std::move(installation.sourceScene);
    m_bindings=std::move(installation.bindings);
    m_animationTextBase.clear();
    m_textEffectBase.clear();
    m_showDiagnostics=installation.diagnosticOverlay;
    ConfigureControlRuntime();
    m_behaviors.SetServices(BehaviorServices());
    m_width=m_height=0;
    return Status::Ok();
}

void UIContext::SetControlRuntimeConfigurator(std::function<void(Control&)> configure){m_configureControlRuntime=std::move(configure);ConfigureControlRuntime();}
void UIContext::SetTextRenderer(graphics::Renderer2D* renderer){m_textRenderer=renderer;ConfigureControlRuntime();if(m_root)m_root->InvalidateLayout();}

Status UIContext::ApplyAnimationProperty(
    const animation::TrackBinding& binding, const Variant& value) {
    Control* control = binding.target == "$root"
                           ? m_root.get()
                           : FindControlByName(m_root.get(), binding.target);
    if (!control)
        return AnimationPropertyFailure(
            "PXUI2417", "Animation UI target was not found: " + binding.target);
    const auto number = [&]() -> std::optional<double> {
        if (const auto* real = value.TryGet<double>()) return *real;
        if (const auto* integer = value.TryGet<std::int64_t>())
            return static_cast<double>(*integer);
        return std::nullopt;
    }();
    if (binding.kind == animation::TargetKind::Text && number) {
        const float progress =
            static_cast<float>(std::clamp(*number, 0.0, 1.0));
        if (binding.property == "typewriter") {
            auto* label = dynamic_cast<Label*>(control);
            if (!label)
                return AnimationPropertyFailure(
                    "PXUI2418",
                    "typewriter requires a Label-compatible text target");
            auto [iterator, inserted] =
                m_animationTextBase.try_emplace(binding.target, label->Text());
            (void)inserted;
            const auto boundaries = text::GraphemeBoundaries(iterator->second);
            const std::size_t visible = static_cast<std::size_t>(
                progress * static_cast<float>(boundaries.size() - 1));
            label->SetText(iterator->second.substr(0, boundaries[visible]));
            if (progress >= 1.0f) m_animationTextBase.erase(iterator);
            return Status::Ok();
        }

        const std::string key = binding.target + "|" + binding.property;
        auto [base, inserted] = m_textEffectBase.try_emplace(
            key, TextEffectBase{control->Offsets(), control->Scale(),
                                control->Modulate(), control->Opacity()});
        (void)inserted;
        const auto finish = [&] {
            if (progress >= 1.0f) m_textEffectBase.erase(key);
        };
        if (binding.property == "fade") {
            control->SetOpacity(base->second.opacity * progress);
            finish();
            return Status::Ok();
        }
        if (binding.property == "slide") {
            Rect offsets = base->second.offsets;
            offsets.x -= (1.0f - progress) * 36.0f;
            control->SetOffsets(offsets);
            finish();
            return Status::Ok();
        }
        if (binding.property == "pop") {
            const float factor = 0.72f + 0.28f * progress +
                                 std::sin(progress * 3.14159265f) * 0.12f;
            control->SetScale({base->second.scale.x * factor,
                               base->second.scale.y * factor});
            finish();
            return Status::Ok();
        }
        if (binding.property == "shake" || binding.property == "wave") {
            Rect offsets = base->second.offsets;
            const float attenuation = 1.0f - progress;
            offsets.x += std::sin(progress * 71.0f) *
                         (binding.property == "shake" ? 5.0f : 2.0f) *
                         attenuation;
            offsets.y += std::sin(progress * 37.0f) *
                         (binding.property == "wave" ? 5.0f : 2.0f) *
                         attenuation;
            control->SetOffsets(offsets);
            finish();
            return Status::Ok();
        }
        if (binding.property == "rainbow") {
            const float phase = progress * 6.2831853f;
            const auto channel = [](const float phaseOffset) {
                return static_cast<std::uint8_t>(
                    std::clamp(127.5f + 127.5f * std::sin(phaseOffset),
                               0.0f, 255.0f));
            };
            Color tint{channel(phase), channel(phase + 2.0943951f),
                       channel(phase + 4.1887902f),
                       base->second.modulate.a};
            if (progress >= 1.0f) tint = base->second.modulate;
            control->SetModulate(tint);
            finish();
            return Status::Ok();
        }
        if (binding.property == "glitch") {
            Rect offsets = base->second.offsets;
            const int frame = static_cast<int>(progress * 60.0f);
            const float jitter = static_cast<float>(((frame * 17) % 9) - 4);
            offsets.x += jitter;
            offsets.y += static_cast<float>(((frame * 29) % 5) - 2);
            control->SetOffsets(offsets);
            control->SetScale(
                {base->second.scale.x * (1.0f + std::abs(jitter) * 0.006f),
                 base->second.scale.y});
            Color tint = base->second.modulate;
            tint.g = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(tint.g) - std::abs(frame % 31),
                           0, 255));
            if (progress >= 1.0f) {
                control->SetOffsets(base->second.offsets);
                control->SetScale(base->second.scale);
                tint = base->second.modulate;
            }
            control->SetModulate(tint);
            finish();
            return Status::Ok();
        }
    }
    if (binding.kind == animation::TargetKind::UI && number) {
        const float progress =
            static_cast<float>(std::clamp(*number, 0.0, 1.0));
        const bool officialPreset =
            binding.property == "panel-slide" ||
            binding.property == "panel-fade" ||
            binding.property == "panel-scale" ||
            binding.property == "modal-open" ||
            binding.property == "modal-close" ||
            binding.property == "button-hover" ||
            binding.property == "button-press";
        if (officialPreset) {
            if ((binding.property == "button-hover" ||
                 binding.property == "button-press") &&
                dynamic_cast<Button*>(control) == nullptr)
                return AnimationPropertyFailure(
                    "PXUI2420", "Button preset requires a Button-compatible target");
            const std::string key = binding.target + "|" + binding.property;
            auto [base, inserted] = m_textEffectBase.try_emplace(
                key, TextEffectBase{control->Offsets(), control->Scale(),
                                    control->Modulate(), control->Opacity()});
            (void)inserted;
            if (binding.property == "panel-slide") {
                Rect offsets = base->second.offsets;
                offsets.x -= (1.0f - progress) * 52.0f;
                control->SetOffsets(offsets);
            } else if (binding.property == "panel-fade") {
                control->SetOpacity(base->second.opacity * progress);
            } else if (binding.property == "panel-scale") {
                const float factor = 0.9f + 0.1f * progress;
                control->SetScale({base->second.scale.x * factor,
                                   base->second.scale.y * factor});
            } else if (binding.property == "modal-open") {
                const float factor = 0.9f + 0.1f * progress;
                control->SetOpacity(base->second.opacity * progress);
                control->SetScale({base->second.scale.x * factor,
                                   base->second.scale.y * factor});
            } else if (binding.property == "modal-close") {
                const float factor = 1.0f - 0.06f * progress;
                control->SetOpacity(base->second.opacity * (1.0f - progress));
                control->SetScale({base->second.scale.x * factor,
                                   base->second.scale.y * factor});
            } else if (binding.property == "button-hover") {
                const float factor = 1.0f +
                    std::sin(progress * 3.14159265f) * 0.045f;
                control->SetScale({base->second.scale.x * factor,
                                   base->second.scale.y * factor});
            } else if (binding.property == "button-press") {
                const float factor = 1.0f -
                    std::sin(progress * 3.14159265f) * 0.075f;
                control->SetScale({base->second.scale.x * factor,
                                   base->second.scale.y * factor});
            }
            if (progress >= 1.0f) m_textEffectBase.erase(key);
            return Status::Ok();
        }
    }
    std::string property = binding.property;
    const auto* descriptor = TypeRegistry::Global().FindProperty(
        std::string(control->TypeName()), property);
    if (!descriptor || !descriptor->set)
        return AnimationPropertyFailure(
            "PXUI2419", "Animation UI property was not found: " + property);
    return descriptor->set(*control, value);
}

void UIContext::ResetAnimationPropertyOverrides(const std::string_view target) {
    for (auto iterator = m_animationTextBase.begin();
         iterator != m_animationTextBase.end();) {
        if (!target.empty() && iterator->first != target) {
            ++iterator;
            continue;
        }
        if (auto* label = dynamic_cast<Label*>(
                FindControlByName(m_root.get(), iterator->first)))
            label->SetText(iterator->second);
        iterator = m_animationTextBase.erase(iterator);
    }
    for (auto iterator = m_textEffectBase.begin();
         iterator != m_textEffectBase.end();) {
        const std::size_t separator = iterator->first.find('|');
        const std::string_view effectTarget(iterator->first.data(), separator);
        if (!target.empty() && effectTarget != target) {
            ++iterator;
            continue;
        }
        if (auto* control = FindControlByName(m_root.get(), effectTarget)) {
            control->SetOffsets(iterator->second.offsets);
            control->SetScale(iterator->second.scale);
            control->SetModulate(iterator->second.modulate);
            control->SetOpacity(iterator->second.opacity);
        }
        iterator = m_textEffectBase.erase(iterator);
    }
}

void UIContext::ConfigureControlRuntime(){if(!m_root)return;std::function<void(scene::Node&)> visit=[&](scene::Node& node){if(auto* control=dynamic_cast<Control*>(&node)){if(m_textRenderer)control->SetTextLayoutProvider([this](const Control& target,const std::string_view value,const int size,const int wrap,const text::TextOrientation orientation,const std::size_t rows){const auto& style=m_theme.Resolve(target);return m_textRenderer->LayoutText(std::string(value),style.font,size>0?size:style.fontSize,wrap,orientation,rows);});if(m_configureControlRuntime)m_configureControlRuntime(*control);}for(const auto& child:node.Children())visit(*child);};visit(*m_root);}

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
    services.animationPlaying=[this](std::uint64_t handle){if(handle==embeddedAnimation)return m_animationController&&m_animationController->ActivePlayback();return m_externalAnimationPlaying&&m_externalAnimationPlaying(handle);};
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

UIRuntimeState UIContext::CaptureRuntimeState() const {
    UIRuntimeState state;
    state.behavior = CaptureBehaviorState();
    if (m_animationController && m_animationController->Library())
        state.animation = m_animationController->CaptureState();
    if (m_visualStateController)
        state.visualState = m_visualStateController->CaptureState();
    return state;
}

Status UIContext::RestoreRuntimeState(const UIRuntimeState& state) {
    if (const Status valid = ValidateRuntimeState(state); !valid) return valid;
    const UIRuntimeState previous = CaptureRuntimeState();
    const auto apply = [this](const UIRuntimeState& value) -> Status {
        if (const Status behavior = RestoreBehaviorState(value.behavior); !behavior)
            return behavior;
        if (value.animation) {
            if (const Status animation=m_animationController->RestoreState(*value.animation);
                !animation)return animation;
        }
        if (value.visualState) {
            if (const Status visual=m_visualStateController->RestoreState(*value.visualState);
                !visual)return visual;
        }
        return Status::Ok();
    };
    const Status restored = apply(state);
    if (restored) return restored;
    const Status rolledBack = apply(previous);
    if (!rolledBack)
        diag::Emit(diag::Diagnostic{.severity=diag::Severity::Fatal,.code="PXUI2416",
            .category="UI.Runtime",.message="UI checkpoint failed and the active UI state could not be recovered"});
    return restored;
}

Status UIContext::ValidateRuntimeState(const UIRuntimeState& state) {
    if (const Status actions=m_actions.ValidateState(state.behavior.actions);!actions)return actions;
    if (const Status fibers=m_behaviors.ValidateState(state.behavior.fibers);!fibers)return fibers;
    if (state.animation) {
        if (!m_animationController || !m_animationController->Library())
            return Status::Fail(diag::Diagnostic{
                .severity=diag::Severity::Error,.code="PXUI2414",
                .category="UI.Runtime",
                .message="UI checkpoint requires an Animation Controller"});
        if (const Status animation=m_animationController->ValidateState(*state.animation);!animation)return animation;
    }
    if (state.visualState) {
        if (!m_visualStateController)
            return Status::Fail(diag::Diagnostic{
                .severity=diag::Severity::Error,.code="PXUI2415",
                .category="UI.Runtime",
                .message="UI checkpoint requires a Visual State Controller"});
        if (const Status visual=m_visualStateController->ValidateState(*state.visualState);!visual)return visual;
    }
    return Status::Ok();
}

Status UIContext::ConfigureTriggers(std::vector<TriggerBinding> triggers,
                                    std::optional<BehaviorGraph> interactionGraph,
                                    std::string sourceScene){
    if(!m_root)return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
        .code="PXUI2403",.category="UI.Runtime",.message="ConfigureTriggers requires a UI root"});
    BehaviorGraphRunner candidate;
    if(interactionGraph){
        const Status status=candidate.SetGraph(*interactionGraph,sourceScene);
        if(!status)return status;
    }
    for(auto& binding:triggers)binding.sourceScene=sourceScene;
    if(const Status status=ValidateTriggers(
           *m_root,triggers,interactionGraph?&*interactionGraph:nullptr);
       !status)return status;
    auto connections=ConnectTriggers(*m_root,triggers);

    // Everything above is detached or additive and fallible. Publish only once
    // validation is complete, then remove the previous source's connections.
    m_behaviors.CancelAll();
    m_actions.CancelSource(m_triggerSourceScene);
    DisconnectTriggers(*m_root,m_triggerConnections);
    m_behaviors=std::move(candidate);
    m_behaviors.SetServices(BehaviorServices());
    m_triggerBindings=std::move(triggers);
    m_triggerConnections=std::move(connections);
    m_triggerSourceScene=std::move(sourceScene);
    return Status::Ok();
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
    if (m_accessibilityAdapter)
        m_accessibilityAdapter->Publish(accessibility::SemanticTreeBuilder::Build(
            *m_root, ++m_accessibilityRevision));
    return m_input && m_input->LastFrameConsumed();
}

accessibility::SemanticTree UIContext::CaptureAccessibilityTree() const {
    return m_root ? accessibility::SemanticTreeBuilder::Build(*m_root, m_accessibilityRevision)
                  : accessibility::SemanticTree{};
}

void UIContext::SetAccessibilityAdapter(
    std::shared_ptr<accessibility::SemanticAdapter> adapter) {
    if (adapter) {
        adapter->SetActionHandler(
            [this](const Uuid& id, const std::string_view action,
                   const std::string_view value) {
                if (!m_root) return false;
                auto* control = dynamic_cast<Control*>(m_root->Find(id));
                if (!control) return false;
                if (action == "focus") {
                    if (!m_input || control->GetFocusMode() == FocusMode::None ||
                        !control->Enabled() || !control->IsVisibleInTree())
                        return false;
                    m_input->SetFocus(control);
                    return true;
                }
                return control->PerformAccessibilityAction(action, value);
            });
        if (m_root)
            adapter->Publish(accessibility::SemanticTreeBuilder::Build(
                *m_root, ++m_accessibilityRevision));
    }
    m_accessibilityAdapter = std::move(adapter);
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
