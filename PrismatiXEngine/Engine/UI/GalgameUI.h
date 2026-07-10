#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/UIContext.h"
#include "Engine/Resources/TypedDocument.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace px { class Input; }
namespace px::graphics { class Renderer2D; }

namespace px::ui {

class Label;
class VBoxContainer;

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
    std::vector<std::string> choices;
    std::vector<std::string> nvlLines;
    bool nvlMode = false;
    bool autoMode = false;
    bool skipMode = false;
};
struct SettingsPresentation {
    int bgm = 128;
    int se = 128;
    int voice = 128;
    int textSpeedMs = 30;
    bool skipReadOnly = true;
    bool fullscreen = false;
};

class GalgameUI {
public:
    enum class Screen { Title, HUD, Backlog, Save, Load, Gallery, Settings, Video };
    using ActionSink = std::function<void(const GalgameAction&)>;

    GalgameUI();
    Status RegisterTemplate(Screen screen,std::string_view typedScene,const std::string& sourcePath={});
    void SetActionSink(ActionSink sink) { m_sink = std::move(sink); }
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

private:
    class ItemSource;
    void Emit(std::string command, std::string argument = {});
    Status Install(std::unique_ptr<Control> root, Screen screen);
    Status InstallTemplate(Screen screen);
    [[nodiscard]] bool HasTemplate(Screen screen) const;
    std::unique_ptr<Control> MakeRoot(std::string name);
    std::unique_ptr<Control> MakeMenuButton(std::string text, std::string command, std::string argument = {});
    Status Add(Control& parent, std::unique_ptr<Control> child);

    UIContext m_context;
    ActionSink m_sink;
    Screen m_screen = Screen::Title;
    Label* m_speaker = nullptr;
    Label* m_dialogue = nullptr;
    Label* m_nvlText = nullptr;
    VBoxContainer* m_choices = nullptr;
    Label* m_mode = nullptr;
    std::shared_ptr<ItemSource> m_items;
    ObservableViewModel m_viewModel;
    std::vector<Binding> m_bindings;
    std::unordered_map<int,resource::TypedDocument> m_templates;
    std::vector<GalgameAction> m_pendingActions;
};

}  // namespace px::ui
