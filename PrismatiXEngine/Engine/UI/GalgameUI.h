#pragma once

#include "Engine/Core/Result.h"
#include "Engine/SDK/Ui.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UiAdapter.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/Animation/Timeline.h"

#include <functional>
#include <cstddef>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px { class Input; }
namespace px::graphics { class Renderer2D; }

namespace px::ui {

class Label;
class VBoxContainer;
class EdgeRevealContainer;

struct GalgameAction { std::string command; std::string argument; };
struct GalgameItem {
    std::string key;
    std::string title;
    std::string subtitle;
    std::string image;
    bool disabled = false;
    std::string command;
    std::string argument;
};
struct DialoguePresentation {
    std::string speaker;
    std::string text;
    std::string fullText;
    std::string chapterTitle;
    std::string musicTitle;
    std::string effect;
    float effectProgress = 0.0f;
    std::vector<std::string> choices;
    std::vector<std::string> nvlLines;
    bool nvlMode = false;
    bool autoMode = false;
    bool skipMode = false;
    float textScale = 1.0f;
    bool reducedMotion = false;
};
struct SettingsPresentation {
    int master = 128;
    int bgm = 128;
    int se = 128;
    int voice = 128;
    std::map<std::string, int, std::less<>> voiceProfiles;
    std::map<std::string, bool, std::less<>> voiceProfileMuted;
    bool masterMuted = false;
    bool bgmMuted = false;
    bool seMuted = false;
    bool voiceMuted = false;
    bool duckBgmOnVoice = true;
    bool continueVoiceOnAdvance = false;
    int textSpeedMs = 30;
    bool skipReadOnly = true;
    bool fullscreen = false;
    float textScale = 1.0f;
    bool highContrast = false;
    bool reducedMotion = false;
    bool selfVoicing = false;
    std::string language;
    std::vector<std::string> languages;
};

class GalgameUI {
public:
    // Screen remains the compatibility vocabulary for the default VN
    // controllers. Arbitrary authored route ids use Custom and never need to
    // enter this enum.
    enum class Screen { Title, HUD, Backlog, Save, Load, Gallery, Settings, Video, Custom };
    using ActionSink = std::function<void(const GalgameAction&)>;

    GalgameUI();
    ~GalgameUI();
    // Production templates use the canonical public UI document contract.
    Status RegisterTemplate(Screen screen,std::string_view scene,
                            const std::string& sourcePath={});
    Status RegisterRouteTemplate(std::string route, std::string_view scene,
                                 const std::string& sourcePath = {});
    [[nodiscard]] Result<std::vector<std::string>> RouteImageDependencies(
        std::string_view route) const;
    [[nodiscard]] std::vector<std::string> MountedImageDependencies() const {
        return m_context.MountedImageDependencies();
    }
    void SetUiAssetResolver(UiAssetResolver resolver) {
        m_studioAssetResolver = std::move(resolver);
    }
    void SetUiComponentLoader(UiComponentLoader loader) {
        m_studioComponentLoader = std::move(loader);
    }
    void SetUiMotionLoader(UiMotionLoader loader) {
        m_studioMotionLoader = std::move(loader);
    }
    void SetActionSink(ActionSink sink) { m_sink = std::move(sink); }
    /** Define an authored UI ViewModel value before its route is mounted. */
    Status DefineUiData(std::string path, Variant value);
    /** Update host data after boot; declared UI values are checked before commit. */
    Status UpdateUiData(std::string_view path, const Variant& value);
    /** Register a host formatter before mounting routes that reference it. */
    Status RegisterUiFormatter(Formatter formatter);
    /** Check and retain the author's host contract for later data updates. */
    [[nodiscard]] Status ValidateUiContract(std::string_view document);
    // Makes an authored command available to a game-specific UI route and
    // forwards its optional string argument to the normal Galgame action
    // sink.  Games no longer need engine-side hard-coded command whitelists
    // just to make a skinned screen interactive.
    Status RegisterActionCommand(std::string command,
                                 std::string argumentKey = "argument");
    /** Register a typed host Action with an explicit native handler. */
    Status RegisterHostAction(ActionDescriptor descriptor,
                              std::function<Status(const ActionInvocation&)> handler);
    /** Check declared host Actions before mounting authored routes. */
    [[nodiscard]] Status ValidateActionContract(std::string_view document);
    void SetBehaviorVariableAccess(
        std::function<std::optional<Variant>(std::string_view)> read,
        std::function<Status(std::string_view, const Variant&)> write) {
        m_context.SetBehaviorVariableAccess(std::move(read), std::move(write));
    }
    void SetExternalAnimationServices(
        std::function<Result<std::uint64_t>(std::string_view)> play,
        std::function<bool(std::uint64_t)> playing) {
        m_context.SetExternalAnimationServices(std::move(play),std::move(playing));
    }
    void SetControlRuntimeConfigurator(std::function<void(Control&)> configure) {
        m_context.SetControlRuntimeConfigurator(std::move(configure));
    }
    void SetMotionAudioSink(std::function<void(const sdk::MotionAudioMarker&)> sink) {
        m_context.SetMotionAudioSink(std::move(sink));
    }
    void SetInputBindingService(Input* input,
                                std::function<void()> onChanged = {}) {
        m_context.SetInputBindingService(input, std::move(onChanged));
    }
    void SetTextRenderer(graphics::Renderer2D* renderer) {
        m_context.SetTextRenderer(renderer);
    }
    Status SetDefaultFont(std::string path);
    void SetImagePreloadSink(std::function<void(std::string_view)> sink) {
        m_context.SetImagePreloadSink(std::move(sink));
    }
    void SetAccessibilityAdapter(
        std::shared_ptr<accessibility::SemanticAdapter> adapter) {
        m_context.SetAccessibilityAdapter(std::move(adapter));
    }
    [[nodiscard]] BehaviorRuntimeState CaptureBehaviorState() const {
        return m_context.CaptureBehaviorState();
    }
    Status RestoreBehaviorState(const BehaviorRuntimeState& state) {
        return m_context.RestoreBehaviorState(state);
    }
    [[nodiscard]] UIRuntimeState CaptureRuntimeState() const;
    // Save/rollback checkpoints always describe gameplay HUD state, even when
    // the user opened a transient save, backlog, gallery, or settings surface.
    [[nodiscard]] UIRuntimeState CaptureGameplayRuntimeState() const;
    Status ValidateRuntimeState(const UIRuntimeState& state);
    Status RestoreRuntimeState(const UIRuntimeState& state);
    [[nodiscard]] ActionDispatcher& Actions() { return m_context.Actions(); }
    Status ShowRoute(std::string_view route);
    Status ShowTitle();
    Status ShowHUD(const DialoguePresentation& presentation);
    Status ShowBacklog(std::vector<GalgameItem> entries);
    Status ShowSaveLoad(bool saveMode, std::vector<GalgameItem> slots);
    enum class SaveLoadPrompt { None, Save, Load, Overwrite };
    [[nodiscard]] bool SupportsSaveLoadConfirmation() const;
    Status SetSaveLoadConfirmation(SaveLoadPrompt prompt);
    [[nodiscard]] bool SupportsTitleConfirmation() const;
    Status SetTitleConfirmation(bool visible);
    enum class SystemPrompt { None, Reset, Exit };
    [[nodiscard]] bool SupportsSystemConfirmation() const;
    Status SetSystemConfirmation(SystemPrompt prompt);
    Status ShowGallery(std::vector<GalgameItem> entries);
    Status ShowSettings(const SettingsPresentation& settings);
    Status ShowVideoOverlay(bool skippable);
    Status RefreshHUD(const DialoguePresentation& presentation);
    Status ToggleHudToolbarPinned();
    Status ActivateChoice(std::size_t index);
    // Preview tooling uses the same Runtime control tree for toolbar/context
    // activation as pointer input. This keeps authored Action dispatch out of
    // JavaScript while preserving exact node identity.
    Status ActivateUiControl(std::string_view nodeId);
    Status SetUiControlVisibility(std::string_view nodeId, bool visible);
    Status PreviewUiAnimation(std::string_view clipId, float time,
                                  bool playing);
    Status StopUiAnimation(bool restoreDesignState);
    void ResetUiTimelineOverrides();

    [[nodiscard]] bool Update(const Input& input, int width, int height,float deltaSeconds=0.0f);
    void Render(graphics::Renderer2D& renderer);
    [[nodiscard]] Screen CurrentScreen() const { return m_screen; }
    [[nodiscard]] std::string_view CurrentRoute() const { return m_route; }
    [[nodiscard]] bool IsOverlay() const { return m_screen != Screen::Title && m_screen != Screen::HUD; }
    Status ApplyAnimationProperty(const animation::TrackBinding& binding,const Variant& value);
    Status ValidateAnimationProperty(const animation::TrackBinding& binding,
                                     const Variant& value,
                                     const UIRuntimeState& state);
    [[nodiscard]] Control* Root() { return m_context.Root(); }
    [[nodiscard]] const Control* Root() const { return m_context.Root(); }

private:
    class ItemSource;
    void Emit(std::string command, std::string argument = {});
    Status Install(std::unique_ptr<Control> root, Screen screen,
                   std::string route = {});
    Status InstallTemplate(Screen screen);
    Status InstallCheckpointSurface(Screen screen);
    Status InstallRouteTemplate(std::string_view route);
    Status InstallCheckpointRoute(std::string_view route);
    [[nodiscard]] bool HasTemplate(Screen screen) const;
    [[nodiscard]] bool HasRouteTemplate(std::string_view route) const;
    std::unique_ptr<Control> MakeMenuButton(std::string text, std::string command, std::string argument = {});
    Status Add(Control& parent, std::unique_ptr<Control> child);
    void RememberGameplayCheckpoint(Screen target);
    [[nodiscard]] Status ValidateUiContractForModel(
        std::string_view document, const ObservableViewModel& model) const;

    // Bindings owned by UIContext unsubscribe during context destruction, so
    // the ViewModel must be declared first and therefore outlive it.
    ObservableViewModel m_viewModel;
    std::string m_uiContractDocument;
    UIContext m_context;
    ActionSink m_sink;
    Screen m_screen = Screen::Title;
    std::string m_route = "title";
    Label* m_speaker = nullptr;
    Label* m_dialogue = nullptr;
    int m_dialogueBaseFontSize = 30;
    // The fitted dialogue size depends only on the full line and the box;
    // RefreshHUD runs every frame, so the last answer is kept.
    struct DialogueFit {
        std::string text;
        int requested = 0;
        Rect bounds{};
        int size = 0;
    } m_dialogueFit;
    Label* m_nvlText = nullptr;
    VBoxContainer* m_choices = nullptr;
    std::vector<std::string> m_presentedChoices;
    Label* m_mode = nullptr;
    Label* m_chapterNotice = nullptr;
    Label* m_musicNotice = nullptr;
    EdgeRevealContainer* m_noticePanel = nullptr;
    std::string m_lastChapterTitle;
    std::string m_lastMusicTitle;
    std::string m_activeDialogueEffect;
    bool m_dialogueEffectFinished = false;
    std::shared_ptr<ItemSource> m_items;
    std::vector<Binding> m_bindings;
    struct Template {
        std::optional<sdk::UiDocument> document;
        std::string sourcePath;
    };
    std::unordered_map<std::string,Template> m_templates;
    UiAssetResolver m_studioAssetResolver;
    UiComponentLoader m_studioComponentLoader;
    UiMotionLoader m_studioMotionLoader;
    std::vector<GalgameAction> m_pendingActions;
    std::vector<std::string> m_hostActionSourceIds;
    std::optional<UIRuntimeState> m_lastGameplayState;
    std::unordered_map<std::string, Visibility> m_timelineVisibilityBase;
};

}  // namespace px::ui
