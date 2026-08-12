#pragma once

#include "Engine/Core/Result.h"
#include "Engine/SDK/StudioUi.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/StudioUiAdapter.h"
#include "Engine/UI/StudioUiApplication.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Animation/Timeline.h"

#include <functional>
#include <memory>
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
    int bgm = 128;
    int se = 128;
    int voice = 128;
    int textSpeedMs = 30;
    bool skipReadOnly = true;
    bool fullscreen = false;
    float textScale = 1.0f;
    bool highContrast = false;
    bool reducedMotion = false;
    bool selfVoicing = false;
};

class GalgameUI {
public:
    enum class Screen { Title, HUD, Backlog, Save, Load, Gallery, Settings, Video };
    using ActionSink = std::function<void(const GalgameAction&)>;

    GalgameUI();
    // Accepts either the Engine typed UIScene or the public Studio UI contract.
    // Both formats install into the same UIContext and Action runtime.
    Status RegisterTemplate(Screen screen,std::string_view scene,
                            const std::string& sourcePath={});
    void SetStudioUiAssetResolver(StudioUiAssetResolver resolver) {
        m_studioAssetResolver = std::move(resolver);
    }
    void SetStudioUiComponentLoader(StudioUiComponentLoader loader) {
        m_studioComponentLoader = std::move(loader);
    }
    void SetActionSink(ActionSink sink) { m_sink = std::move(sink); }
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
    [[nodiscard]] BehaviorRuntimeState CaptureBehaviorState() const {
        return m_context.CaptureBehaviorState();
    }
    Status RestoreBehaviorState(const BehaviorRuntimeState& state) {
        return m_context.RestoreBehaviorState(state);
    }
    [[nodiscard]] ActionDispatcher& Actions() { return m_context.Actions(); }
    Status ShowTitle();
    Status ShowHUD(const DialoguePresentation& presentation);
    Status ShowBacklog(std::vector<GalgameItem> entries);
    Status ShowSaveLoad(bool saveMode, std::vector<GalgameItem> slots);
    Status ShowGallery(std::vector<GalgameItem> entries);
    Status ShowSettings(const SettingsPresentation& settings);
    Status ShowVideoOverlay(bool skippable);
    Status RefreshHUD(const DialoguePresentation& presentation);

    [[nodiscard]] bool Update(const Input& input, int width, int height,float deltaSeconds=0.0f);
    void Render(graphics::Renderer2D& renderer);
    [[nodiscard]] Screen CurrentScreen() const { return m_screen; }
    [[nodiscard]] bool IsOverlay() const { return m_screen != Screen::Title && m_screen != Screen::HUD; }
    Status ApplyAnimationProperty(const animation::TrackBinding& binding,const Variant& value);

private:
    class ItemSource;
    void Emit(std::string command, std::string argument = {});
    Status Install(std::unique_ptr<Control> root, Screen screen);
    Status InstallTemplate(Screen screen);
    [[nodiscard]] bool HasTemplate(Screen screen) const;
    std::unique_ptr<Control> MakeRoot(std::string name);
    std::unique_ptr<Control> MakeMenuButton(std::string text, std::string command, std::string argument = {});
    Status Add(Control& parent, std::unique_ptr<Control> child);

    // Bindings owned by UIContext unsubscribe during context destruction, so
    // the ViewModel must be declared first and therefore outlive it.
    ObservableViewModel m_viewModel;
    UIContext m_context;
    ActionSink m_sink;
    Screen m_screen = Screen::Title;
    Label* m_speaker = nullptr;
    Label* m_dialogue = nullptr;
    Rect m_dialogueBaseOffsets{};
    int m_dialogueBaseFontSize = 30;
    Label* m_nvlText = nullptr;
    VBoxContainer* m_choices = nullptr;
    Label* m_mode = nullptr;
    Label* m_chapterNotice = nullptr;
    Label* m_musicNotice = nullptr;
    EdgeRevealContainer* m_noticePanel = nullptr;
    std::string m_lastChapterTitle;
    std::string m_lastMusicTitle;
    std::shared_ptr<ItemSource> m_items;
    std::vector<Binding> m_bindings;
    struct Template {
        std::optional<resource::TypedDocument> typedScene;
        std::optional<sdk::StudioUiDocument> studioScene;
        std::string sourcePath;
    };
    std::unordered_map<int,Template> m_templates;
    StudioUiAssetResolver m_studioAssetResolver;
    StudioUiComponentLoader m_studioComponentLoader;
    std::vector<GalgameAction> m_pendingActions;
    std::unordered_map<std::string,std::string> m_animationTextBase;
};

}  // namespace px::ui
