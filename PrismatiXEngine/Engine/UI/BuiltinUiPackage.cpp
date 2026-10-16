#include "Engine/UI/BuiltinUiPackage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace px::ui {
namespace {

// Screens are designed on a 1280x720 canvas and scaled to the project's
// logical resolution when they are installed.
constexpr float kDesignWidth = 1280.0f;
constexpr float kDesignHeight = 720.0f;

// Minimal and monochrome: translucent black over the game, white text at a
// few opacities, text-only buttons and one hairline under each heading.
// Nothing competes with the game's own art.
constexpr char kTitleBackground[] = "#0C0C0EFF";
constexpr char kOverlay[] = "#000000D9";
constexpr char kWindowTop[] = "#0000008C";
constexpr char kWindowBottom[] = "#000000BF";
constexpr char kText[] = "#FFFFFFEB";
constexpr char kMuted[] = "#FFFFFF99";
constexpr char kFaint[] = "#FFFFFF66";
constexpr char kLine[] = "#FFFFFF24";
constexpr char kHover[] = "#FFFFFF14";
constexpr char kFocus[] = "#FFFFFFCC";

// Stable node ids: 7<screen>000000-0000-4000-8000-<serial>.
std::string Id(const int screen, const int serial) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "7%d000000-0000-4000-8000-%012x", screen, serial);
    return buffer;
}

sdk::UiLayout Layout(const float x, const float y, const float width,
                     const float height, const bool fill = false) {
    sdk::UiLayout layout;
    layout.mode = sdk::UiLayoutMode::Free;
    layout.x = x;
    layout.y = y;
    layout.width = width;
    layout.height = height;
    layout.anchorRight = fill ? 1.0f : 0.0f;
    layout.anchorBottom = fill ? 1.0f : 0.0f;
    layout.alignment = fill ? "fill" : "start";
    layout.sizeRule = fill ? "fill" : "fixed";
    return layout;
}

// Builds one screen. Order values increase in authoring order, so later
// nodes draw over earlier siblings.
class Screen {
public:
    Screen(const int screen, std::string documentId, std::string name)
        : m_screen(screen), m_documentId(std::move(documentId)), m_name(std::move(name)) {}

    sdk::UiNode& Node(std::optional<std::string> parent, const sdk::UiNodeKind kind,
                      std::string runtimeType, std::string name,
                      const sdk::UiLayout& layout, std::string role = "group",
                      std::string label = {}) {
        sdk::UiNode node;
        node.id = Id(m_screen, ++m_serial);
        node.parentId = std::move(parent);
        node.order = static_cast<std::uint32_t>(m_serial);
        node.kind = kind;
        if (!runtimeType.empty()) node.runtimeType = std::move(runtimeType);
        node.name = std::move(name);
        node.layout = layout;
        node.accessibilityRole = std::move(role);
        node.accessibilityLabel = std::move(label);
        node.accessibilityFocusOrder = m_serial;
        m_nodes.push_back(std::move(node));
        return m_nodes.back();
    }

    // A flat fill: a scrim, a band or a hairline.
    sdk::UiNode& Box(std::optional<std::string> parent, std::string name,
                     const sdk::UiLayout& layout, const char* color) {
        auto& node = Node(std::move(parent), sdk::UiNodeKind::Group, {}, std::move(name), layout);
        node.appearance.backgroundColor = color;
        return node;
    }

    sdk::UiNode& Label(const std::string& parent, std::string type, std::string name,
                       const sdk::UiLayout& layout, std::string text,
                       const std::int64_t fontSize, const char* color = kText,
                       std::string role = "text") {
        auto& node = Node(parent, sdk::UiNodeKind::Label, std::move(type), std::move(name),
                          layout, std::move(role), text);
        node.text = std::move(text);
        node.appearance.textColor = color;
        node.runtimeProperties.emplace("fontSize", fontSize);
        return node;
    }

    // A text-only button: muted at rest, white on a faint wash when hovered.
    sdk::UiNode& TextButton(const std::string& parent, std::string name,
                            const sdk::UiLayout& layout, std::string text,
                            std::string action, const std::int64_t fontSize,
                            const bool alignLeft = false, const char* color = kMuted) {
        auto& node = Node(parent, sdk::UiNodeKind::Button, {}, std::move(name), layout,
                          "button", text);
        node.text = std::move(text);
        node.onClick = sdk::UiAction{.id = std::move(action)};
        node.appearance.backgroundColor = "#00000000";
        node.appearance.hoverBackgroundColor = kHover;
        node.appearance.textColor = color;
        node.appearance.hoverTextColor = "#FFFFFFFF";
        node.appearance.focusColor = kFocus;
        node.appearance.cornerRadius = 3.0f;
        node.runtimeProperties.emplace("focusMode", std::string("All"));
        node.runtimeProperties.emplace("fontSize", fontSize);
        if (alignLeft) node.runtimeProperties.emplace("horizontalAlignment", std::string("Left"));
        return node;
    }

    // Translucent black over whatever the game shows.
    std::string Overlay(std::string name, std::string role, std::string label) {
        auto& root = Box(std::nullopt, std::move(name),
                         Layout(0, 0, kDesignWidth, kDesignHeight, true), kOverlay);
        root.accessibilityRole = std::move(role);
        root.accessibilityLabel = std::move(label);
        return root.id;
    }

    // The screen title and a hairline across the page.
    void Heading(const std::string& parent, std::string text) {
        Label(parent, "Label", "ScreenHeading", Layout(64, 40, 800, 40), std::move(text), 26,
              kText, "heading");
        Box(parent, "HeadingRule", Layout(64, 92, 1152, 1), kLine);
    }

    sdk::UiDocument Finish() {
        sdk::UiDocument document;
        document.id = m_documentId;
        document.revision = 1;
        document.name = m_name;
        document.width = static_cast<int>(kDesignWidth);
        document.height = static_cast<int>(kDesignHeight);
        document.rootId = m_nodes.front().id;
        document.nodes = std::move(m_nodes);
        document.theme = {
            {"dialogue.background", "Dialogue background", kWindowBottom},
            {"focus.color", "Focus color", kFocus}};
        return document;
    }

private:
    int m_screen;
    int m_serial = 0;
    std::string m_documentId;
    std::string m_name;
    std::vector<sdk::UiNode> m_nodes;
};

sdk::UiDocument Title() {
    Screen screen(1, "70000000-0000-4000-8000-000000000001", "PrismatiX Builtin Title");
    auto& root = screen.Box(std::nullopt, "TitleRoot",
                            Layout(0, 0, kDesignWidth, kDesignHeight, true), kTitleBackground);
    root.accessibilityRole = "window";
    root.accessibilityLabel = "Game title";
    const std::string id = root.id;
    screen.Label(id, "Label", "GameTitle", Layout(96, 236, 1000, 70), "PrismatiX", 48, kText,
                 "heading");
    const std::pair<const char*, const char*> entries[]{
        {"開始遊戲", "game.start"}, {"繼續遊戲", "game.continue"},
        {"讀取進度", "load.open"}, {"CG 鑑賞", "gallery.open"},
        {"設定", "settings.open"}, {"結束", "app.quit"}};
    const char* names[]{"Start", "Continue", "Load", "Gallery", "Settings", "Quit"};
    for (std::size_t index = 0; index < std::size(entries); ++index)
        screen.TextButton(id, names[index],
                          Layout(88, 336.0f + static_cast<float>(index) * 44.0f, 220, 36),
                          entries[index].first, entries[index].second, 18, true);
    screen.Label(id, "Label", "Credit", Layout(96, 676, 400, 20), "Made with PrismatiX", 12,
                 kFaint);
    return screen.Finish();
}

sdk::UiDocument Hud() {
    Screen screen(2, "70000000-0000-4000-8000-000000000002", "PrismatiX Builtin HUD");
    const std::string root = screen.Node(std::nullopt, sdk::UiNodeKind::Group, "Container",
                                         "HUDRoot", Layout(0, 0, kDesignWidth, kDesignHeight, true),
                                         "window", "Game HUD").id;
    // Full-screen page for [nvl].
    const std::string nvl = screen.Box(root, "NVLPanel",
                                       Layout(0, 0, kDesignWidth, kDesignHeight), "#000000B3").id;
    auto& nvlText = screen.Label(nvl, "RichTextLabel", "NVLText",
                                 Layout(176, 72, 928, 576), "", 22, kText, "log");
    nvlText.runtimeProperties.emplace("wrap", true);
    // The message window: a translucent black band across the bottom.
    auto& window = screen.Box(root, "ADVPanel", Layout(0, 540, 1280, 180), kWindowTop);
    window.appearance.gradientColor = kWindowBottom;
    window.accessibilityLabel = "Dialogue panel";
    const std::string adv = window.id;
    screen.Label(adv, "Label", "Speaker", Layout(176, 18, 520, 28), "", 20, kText, "heading");
    auto& dialogue = screen.Label(adv, "RichTextLabel", "Dialogue",
                                  Layout(176, 52, 928, 108), "", 22);
    dialogue.runtimeProperties.emplace("wrap", true);
    // Click-wait mark, shown once the line is complete.
    auto& glyph = screen.Label(adv, "Label", "WaitGlyph", Layout(1110, 146, 16, 16), "▼", 10,
                               kMuted, "status");
    glyph.visible = false;
    glyph.bindings.emplace("visible", sdk::UiPropertyBinding{"dialogue.waiting", {}});
    glyph.runtimeProperties.emplace("showTransitionSeconds", 0.3);
    glyph.runtimeProperties.emplace("showOffsetY", -4.0);
    // Quick menu: small text along the window's top right.
    const std::pair<const char*, const char*> actions[]{
        {"AUTO", "mode.auto"}, {"SKIP", "mode.skip"}, {"LOG", "backlog.open"},
        {"Q.SAVE", "save.quick"}, {"Q.LOAD", "load.quick"}, {"SAVE", "save.open"},
        {"LOAD", "load.open"}, {"CONFIG", "settings.open"}, {"HIDE", "hud.hide"}};
    const float firstX = 1280.0f - 24.0f - static_cast<float>(std::size(actions)) * 60.0f;
    for (std::size_t index = 0; index < std::size(actions); ++index)
        screen.TextButton(adv, "Quick" + std::to_string(index),
                          Layout(firstX + static_cast<float>(index) * 60.0f, 13, 58, 24),
                          actions[index].first, actions[index].second, 12, false, kFaint);
    auto& choices = screen.Node(root, sdk::UiNodeKind::VBox, "VBoxContainer", "Choices",
                                Layout(340, 180, 600, 300), "listbox", "Choices");
    choices.runtimeProperties.emplace("separation", std::int64_t{10});
    auto& mode = screen.Label(root, "Label", "ModeState", Layout(1040, 20, 216, 20), "", 12, kMuted);
    mode.runtimeProperties.emplace("horizontalAlignment", std::string("Right"));
    // Story [input]: a prompt, a text field and OK, shown while the
    // Player waits for a name.
    auto& input = screen.Box(root, "TextInputPanel", Layout(390, 270, 500, 170), kOverlay);
    input.appearance.cornerRadius = 4.0f;
    input.visible = false;
    input.bindings.emplace("visible", sdk::UiPropertyBinding{"input.visible", {}});
    input.accessibilityRole = "dialog";
    input.accessibilityLabel = "Text input";
    const std::string inputId = input.id;
    auto& prompt = screen.Label(inputId, "Label", "TextInputPrompt",
                                Layout(28, 22, 444, 30), "", 18);
    prompt.bindings.emplace("text", sdk::UiPropertyBinding{"input.prompt", {}});
    auto& field = screen.Node(inputId, sdk::UiNodeKind::Control, "LineEdit", "TextInput",
                              Layout(28, 62, 444, 40), "textbox", "Text");
    field.appearance.backgroundColor = kHover;
    field.appearance.textColor = kText;
    field.appearance.focusColor = kFocus;
    screen.TextButton(inputId, "TextInputAccept", Layout(392, 118, 80, 34), "OK",
                      "input.accept", 16, false, kText);
    return screen.Finish();
}

sdk::UiDocument Collection(const int screenNumber, const std::string& documentId,
                           std::string name, std::string heading,
                           std::string runtimeType, std::string listName,
                           const float columns, const sdk::UiVec2Value extent,
                           const sdk::UiVec2Value gap) {
    Screen screen(screenNumber, documentId, "PrismatiX Builtin " + name);
    const std::string root = screen.Overlay(name + "Root", "dialog", name);
    screen.Heading(root, std::move(heading));
    const auto kind = runtimeType == "ListView" ? sdk::UiNodeKind::Control
                                                : sdk::UiNodeKind::Grid;
    auto& list = screen.Node(root, kind, std::move(runtimeType), std::move(listName),
                             Layout(64, 116, 1152, 528), "list", name + " entries");
    if (list.runtimeType != "ListView")
        list.runtimeProperties.emplace("columns", static_cast<double>(columns));
    list.runtimeProperties.emplace("itemExtent", extent);
    list.runtimeProperties.emplace("gap", gap);
    screen.TextButton(root, "Close", Layout(1136, 662, 80, 34), "返回", "overlay.close", 16);
    return screen.Finish();
}

sdk::UiDocument Settings() {
    Screen screen(6, "70000000-0000-4000-8000-000000000006", "PrismatiX Builtin Settings");
    const std::string root = screen.Overlay("SettingsRoot", "dialog", "Settings");
    screen.Heading(root, "設定");
    screen.Label(root, "Label", "SoundHeading", Layout(64, 120, 400, 20), "聲音與文字", 13, kFaint);
    const std::pair<const char*, const char*> sliders[]{
        {"BGM", "音樂"}, {"SE", "音效"}, {"Voice", "語音"},
        {"TextSpeed", "文字速度"}, {"TextScale", "文字大小"}};
    for (std::size_t index = 0; index < std::size(sliders); ++index) {
        const float y = 152.0f + static_cast<float>(index) * 56.0f;
        screen.Label(root, "Label", std::string(sliders[index].first) + "Label",
                     Layout(64, y, 150, 32), sliders[index].second, 17, kMuted);
        screen.Node(root, sdk::UiNodeKind::Control, "Slider", sliders[index].first,
                    Layout(220, y, 420, 32), "slider", sliders[index].second);
    }
    screen.Label(root, "Label", "DisplayHeading", Layout(720, 120, 400, 20), "顯示與輔助", 13,
                 kFaint);
    screen.Label(root, "Label", "LanguageLabel", Layout(720, 152, 180, 32), "語言 / Language", 17,
                 kMuted);
    screen.Node(root, sdk::UiNodeKind::Control, "OptionButton", "Language",
                Layout(908, 148, 308, 40), "combobox", "Language");
    const std::pair<const char*, const char*> checks[]{
        {"SkipRead", "只跳過已讀文字"}, {"Fullscreen", "全螢幕"},
        {"HighContrast", "高對比"}, {"ReducedMotion", "減少動態效果"},
        {"SelfVoicing", "朗讀文字"}};
    for (std::size_t index = 0; index < std::size(checks); ++index) {
        auto& check = screen.Node(root, sdk::UiNodeKind::Button, "CheckBox", checks[index].first,
                                  Layout(716, 208.0f + static_cast<float>(index) * 52.0f, 500, 42),
                                  "checkbox", checks[index].second);
        check.text = checks[index].second;
        check.runtimeProperties.emplace("fontSize", std::int64_t{17});
    }
    screen.TextButton(root, "Close", Layout(1136, 662, 80, 34), "完成", "overlay.close", 16);
    return screen.Finish();
}

sdk::UiDocument Video() {
    Screen screen(7, "70000000-0000-4000-8000-000000000007", "PrismatiX Builtin Video");
    const std::string root = screen.Node(std::nullopt, sdk::UiNodeKind::Group, "Container",
                                         "VideoRoot", Layout(0, 0, kDesignWidth, kDesignHeight, true),
                                         "window", "Video overlay").id;
    screen.Label(root, "Label", "SkipHint", Layout(960, 660, 280, 40), "", 16, kMuted);
    return screen.Finish();
}

void ScaleDocument(sdk::UiDocument& document, const int width, const int height) {
    const float sx = static_cast<float>(width) / kDesignWidth;
    const float sy = static_cast<float>(height) / kDesignHeight;
    const float sf = std::min(sx, sy);
    document.width = width;
    document.height = height;
    for (auto& node : document.nodes) {
        node.layout.x *= sx;
        node.layout.y *= sy;
        node.layout.width *= sx;
        node.layout.height *= sy;
        auto& box = node.appearance;
        box.cornerRadius *= sf;
        box.borderWidth = box.borderWidth > 0.0f ? std::max(1.0f, box.borderWidth * sf) : 0.0f;
        box.shadowBlur *= sf;
        box.shadowOffsetX *= sf;
        box.shadowOffsetY *= sf;
        for (auto& [name, value] : node.runtimeProperties) {
            if (name == "fontSize" || name == "separation") {
                if (auto* integer = std::get_if<std::int64_t>(&value))
                    *integer = std::max<std::int64_t>(
                        1, std::llround(static_cast<double>(*integer) * sf));
            } else if (name == "letterSpacing" || name == "showOffsetY") {
                if (auto* number = std::get_if<double>(&value)) *number *= sf;
            } else if (name == "itemExtent" || name == "gap") {
                if (auto* vector = std::get_if<sdk::UiVec2Value>(&value)) {
                    vector->x *= sx;
                    vector->y *= sy;
                }
            }
        }
    }
}

}  // namespace

std::vector<BuiltinUiEntry> CreateBuiltinUiPackage(const int width, const int height) {
    std::vector<BuiltinUiEntry> entries{
        {"title", Title()},
        {"hud", Hud()},
        {"backlog", Collection(3, "70000000-0000-4000-8000-000000000003", "Backlog", "履歷",
                               "ListView", "Entries", 1, {1152, 56}, {0, 4})},
        {"save", Collection(4, "70000000-0000-4000-8000-000000000004", "Save", "存檔",
                            "GridView", "Slots", 3, {376, 100}, {12, 12})},
        {"load", Collection(8, "70000000-0000-4000-8000-000000000008", "Load", "讀取",
                            "GridView", "Slots", 3, {376, 100}, {12, 12})},
        {"gallery", Collection(5, "70000000-0000-4000-8000-000000000005", "Gallery", "CG 鑑賞",
                               "GridView", "Items", 4, {279, 160}, {12, 12})},
        {"settings", Settings()},
        {"video", Video()},
    };
    if (width > 0 && height > 0 &&
        (width != static_cast<int>(kDesignWidth) || height != static_cast<int>(kDesignHeight)))
        for (auto& entry : entries) ScaleDocument(entry.document, width, height);
    return entries;
}

}  // namespace px::ui
