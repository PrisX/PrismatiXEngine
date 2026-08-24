#pragma once

#include "Engine/UI/Binding.h"
#include "Engine/UI/InputRouter.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/VisualState.h"
#include "Engine/UI/Actions/ActionDispatcher.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <memory>
#include <optional>
#include <vector>

namespace px { class Input; }
namespace px::graphics { class Renderer2D; }

namespace px::ui {

class UIContext {
public:
    UIContext();
    Status SetRoot(std::unique_ptr<Control> root);
    void SetBindings(std::vector<Binding> bindings) {
        m_bindings = std::move(bindings);
    }
    Status ConfigureTriggers(std::vector<TriggerBinding> triggers,
                             std::optional<BehaviorGraph> interactionGraph = std::nullopt,
                             std::string sourceScene = {});
    void SetBehaviorVariableAccess(
        std::function<std::optional<Variant>(std::string_view)> read,
        std::function<Status(std::string_view, const Variant&)> write);
    void SetExternalAnimationServices(
        std::function<Result<std::uint64_t>(std::string_view)> play,
        std::function<bool(std::uint64_t)> playing) {
        m_playExternalAnimation=std::move(play);m_externalAnimationPlaying=std::move(playing);
        m_behaviors.SetServices(BehaviorServices());
    }
    void SetExternalUIAnimationClipResolver(UIAnimationController::ExternalClipResolver resolver){m_animationResolver=std::move(resolver);if(m_animationController)m_animationController->SetExternalClipResolver(m_animationResolver);}
    void SetControlRuntimeConfigurator(std::function<void(Control&)> configure);
    [[nodiscard]] Control* Root() const { return m_root.get(); }
    [[nodiscard]] Theme& ActiveTheme() { return m_theme; }
    void SetTheme(Theme theme){m_theme=std::move(theme);if(m_root)m_root->InvalidateLayout();}
    [[nodiscard]] CommandRegistry& Commands() { return m_commands; }
    [[nodiscard]] ActionDispatcher& Actions() { return m_actions; }
    [[nodiscard]] FormatterRegistry& Formatters() { return m_formatters; }
    [[nodiscard]] BehaviorGraphRunner& Behaviors() { return m_behaviors; }
    [[nodiscard]] const BehaviorGraphRunner& Behaviors() const { return m_behaviors; }
    [[nodiscard]] BehaviorRuntimeState CaptureBehaviorState() const;
    Status RestoreBehaviorState(const BehaviorRuntimeState& state);

    [[nodiscard]] bool Update(const Input& input, int viewportWidth, int viewportHeight,float deltaSeconds=0.0f);
    Status SetAnimations(UIAnimationLibrary library,bool autoplay=true);
    Status SetVisualStateGroups(std::vector<VisualStateGroup> groups);
    Status SetVisualState(std::string_view group, std::string_view state);
    [[nodiscard]] std::optional<std::string_view> ActiveVisualState(
        std::string_view group) const;
    [[nodiscard]] VisualStateRuntimeState CaptureVisualState() const;
    Status RestoreVisualState(const VisualStateRuntimeState& state);
    Status PlayAnimation(std::string_view state = {});
    Status SetAnimationTrigger(std::string_view parameter);
    Status SetAnimationBool(std::string_view parameter,bool value);
    Status SetAnimationNumber(std::string_view parameter,double value);
    Status SetAnimationParameter(std::string_view parameter, const Variant& value);
    Status TravelAnimationState(std::string_view state,float duration=0.0f);
    Status PreviewAnimation(const Uuid& clip, float time, bool playing);
    Status StopAnimation(bool restoreDesignState = true);
    [[nodiscard]] UIAnimationRuntimeState CaptureAnimationState() const;
    Status RestoreAnimationState(const UIAnimationRuntimeState& state);
    void Render(graphics::Renderer2D& renderer);
    void SetDiagnosticOverlayEnabled(bool value) { m_showDiagnostics = value; }

private:
    [[nodiscard]] BehaviorRuntimeServices BehaviorServices();
    void ConfigureControlRuntime();
    void RenderDiagnosticOverlay(graphics::Renderer2D& renderer);

    std::unique_ptr<Control> m_root;
    std::vector<Binding> m_bindings;
    std::unique_ptr<InputRouter> m_input;
    Theme m_theme;
    FormatterRegistry m_formatters;
    CommandRegistry m_commands;
    ActionDispatcher m_actions;
    BehaviorGraphRunner m_behaviors;
    std::vector<TriggerBinding> m_triggerBindings;
    std::string m_triggerSourceScene;
    std::function<std::optional<Variant>(std::string_view)> m_readBehaviorVariable;
    std::function<Status(std::string_view, const Variant&)> m_writeBehaviorVariable;
    std::function<Result<std::uint64_t>(std::string_view)> m_playExternalAnimation;
    std::function<bool(std::uint64_t)> m_externalAnimationPlaying;
    std::function<void(Control&)> m_configureControlRuntime;
    int m_width = 0;
    int m_height = 0;
    bool m_showDiagnostics = true;
    UIAnimationController::ExternalClipResolver m_animationResolver;
    std::unique_ptr<UIAnimationController> m_animationController;
    std::unique_ptr<VisualStateController> m_visualStateController;
};

}  // namespace px::ui
