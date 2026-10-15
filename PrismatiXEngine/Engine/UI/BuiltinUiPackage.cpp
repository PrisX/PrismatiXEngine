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

// "Nocturne": deep ink glass cards over a night gradient, warm off-white
// text, one champagne-gold accent for names, headings and focus, and soft
// periwinkle light. Every panel is a box decoration, so the stock screens
// need no textures.
constexpr char kBackdropTop[] = "#18203AFF";
constexpr char kBackdropBottom[] = "#06080FFF";
constexpr char kOverlayTop[] = "#141B30F7";
constexpr char kOverlayBottom[] = "#070A13FC";
constexpr char kCardTop[] = "#1C2540E0";
constexpr char kCardBottom[] = "#0D1223EB";
constexpr char kDialogueTop[] = "#141C33DE";
constexpr char kDialogueBottom[] = "#090E1BF2";
constexpr char kHairline[] = "#FFFFFF1F";
constexpr char kShadow[] = "#00000080";
constexpr char kText[] = "#EEF1F7FF";
constexpr char kMuted[] = "#9AA5BCFF";
constexpr char kAccent[] = "#E9CF94FF";
constexpr char kAccentDeep[] = "#C9A464FF";
constexpr char kAccentHover[] = "#F6E6BEFF";
constexpr char kInkText[] = "#161B2CFF";
constexpr char kPill[] = "#FFFFFF0F";
constexpr char kPillHover[] = "#E9CF9429";
constexpr char kPillBorder[] = "#FFFFFF2E";
constexpr char kGlowCool[] = "#6F8DFF22";
constexpr char kGlowWarm[] = "#E9CF940F";

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

    // A decorative group; draws only its box decoration.
    sdk::UiNode& Box(std::optional<std::string> parent, std::string name,
                     const sdk::UiLayout& layout, const char* color) {
        auto& node = Node(std::move(parent), sdk::UiNodeKind::Group, {}, std::move(name), layout);
        node.appearance.backgroundColor = color;
        return node;
    }

    // An ink glass card with a hairline border and a soft drop shadow.
    sdk::UiNode& Card(std::optional<std::string> parent, std::string name,
                      const sdk::UiLayout& layout, const char* top = kCardTop,
                      const char* bottom = kCardBottom, const float radius = 18.0f) {
        auto& node = Box(std::move(parent), std::move(name), layout, top);
        node.appearance.gradientColor = bottom;
        node.appearance.cornerRadius = radius;
        node.appearance.borderWidth = 1.0f;
        node.appearance.borderColor = kHairline;
        node.appearance.shadowColor = kShadow;
        node.appearance.shadowBlur = 28.0f;
        node.appearance.shadowOffsetY = 10.0f;
        return node;
    }

    // A soft ambient light: a transparent circle that only casts its glow.
    void Glow(const std::string& parent, std::string name, const float cx,
              const float cy, const float size, const char* color) {
        auto& node = Box(parent, std::move(name),
                         Layout(cx - size * 0.5f, cy - size * 0.5f, size, size), "#00000000");
        node.appearance.cornerRadius = size * 0.5f;
        node.appearance.shadowColor = color;
        node.appearance.shadowBlur = size * 0.6f;
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

    // A rounded pill button: quiet glass at rest, warm gold light on hover.
    sdk::UiNode& Pill(const std::string& parent, std::string name,
                      const sdk::UiLayout& layout, std::string text,
                      std::string action, const std::int64_t fontSize = 20) {
        auto& node = Node(parent, sdk::UiNodeKind::Button, {}, std::move(name), layout,
                          "button", text);
        node.text = std::move(text);
        node.onClick = sdk::UiAction{.id = std::move(action)};
        node.appearance.backgroundColor = kPill;
        node.appearance.hoverBackgroundColor = kPillHover;
        node.appearance.textColor = kText;
        node.appearance.hoverTextColor = kAccentHover;
        node.appearance.focusColor = kAccent;
        node.appearance.cornerRadius = layout.height * 0.5f;
        node.appearance.borderWidth = 1.0f;
        node.appearance.borderColor = kPillBorder;
        node.appearance.hoverBorderColor = kAccent;
        node.runtimeProperties.emplace("focusMode", std::string("All"));
        node.runtimeProperties.emplace("fontSize", fontSize);
        return node;
    }

    // Text-only button for toolbars: muted text that warms on hover.
    sdk::UiNode& Quiet(const std::string& parent, std::string name,
                       const sdk::UiLayout& layout, std::string text,
                       std::string action, const std::int64_t fontSize) {
        auto& node = Pill(parent, std::move(name), layout, std::move(text),
                          std::move(action), fontSize);
        node.appearance.backgroundColor = "#00000000";
        node.appearance.hoverBackgroundColor = "#FFFFFF14";
        node.appearance.textColor = kMuted;
        node.appearance.borderWidth = 0.0f;
        node.appearance.borderColor.reset();
        node.appearance.hoverBorderColor.reset();
        return node;
    }

    // Night-gradient backdrop filling the screen, with two ambient lights.
    std::string Backdrop(std::string name, std::string role, std::string label,
                         const char* top, const char* bottom) {
        auto& root = Box(std::nullopt, std::move(name),
                         Layout(0, 0, kDesignWidth, kDesignHeight, true), top);
        root.appearance.gradientColor = bottom;
        root.accessibilityRole = std::move(role);
        root.accessibilityLabel = std::move(label);
        const std::string id = root.id;
        Glow(id, "GlowCool", 1060, 120, 520, kGlowCool);
        Glow(id, "GlowWarm", 140, 640, 420, kGlowWarm);
        return id;
    }

    // Small gold eyebrow, the screen title and a short accent rule.
    void Heading(const std::string& parent, std::string eyebrow, std::string text) {
        auto& small = Label(parent, "Label", "ScreenEyebrow", Layout(72, 34, 400, 20),
                            std::move(eyebrow), 13, kAccent);
        small.runtimeProperties.emplace("letterSpacing", 4.0);
        Label(parent, "Label", "ScreenHeading", Layout(72, 54, 800, 48), std::move(text),
              32, "#F6F2EAFF", "heading");
        auto& rule = Box(parent, "HeadingRule", Layout(74, 106, 44, 3), kAccent);
        rule.appearance.cornerRadius = 1.5f;
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
            {"dialogue.background", "Dialogue background", kDialogueTop},
            {"focus.color", "Focus color", kAccent}};
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
    const std::string root = screen.Backdrop("TitleRoot", "window", "Game title",
                                             kBackdropTop, kBackdropBottom);
    auto& eyebrow = screen.Label(root, "Label", "TitleEyebrow", Layout(112, 178, 600, 22),
                                 "A VISUAL NOVEL", 14, kAccent);
    eyebrow.runtimeProperties.emplace("letterSpacing", 6.0);
    screen.Label(root, "Label", "GameTitle", Layout(108, 202, 1000, 90), "PrismatiX", 66,
                 "#F7F3EBFF", "heading");
    auto& rule = screen.Box(root, "TitleRule", Layout(112, 300, 64, 3), kAccent);
    rule.appearance.cornerRadius = 1.5f;
    const std::pair<const char*, const char*> entries[]{
        {"開始遊戲", "game.start"}, {"繼續遊戲", "game.continue"},
        {"讀取進度", "load.open"}, {"CG 鑑賞", "gallery.open"},
        {"設定", "settings.open"}, {"結束", "app.quit"}};
    const char* names[]{"Start", "Continue", "Load", "Gallery", "Settings", "Quit"};
    for (std::size_t index = 0; index < std::size(entries); ++index)
        screen.Pill(root, names[index],
                    Layout(112, 336.0f + static_cast<float>(index) * 54.0f, 280, 42),
                    entries[index].first, entries[index].second, 19);
    screen.Label(root, "Label", "Credit", Layout(112, 668, 400, 22),
                 "Made with PrismatiX", 13, kMuted);
    return screen.Finish();
}

sdk::UiDocument Hud() {
    Screen screen(2, "70000000-0000-4000-8000-000000000002", "PrismatiX Builtin HUD");
    const std::string root = screen.Node(std::nullopt, sdk::UiNodeKind::Group, "Container",
                                         "HUDRoot", Layout(0, 0, kDesignWidth, kDesignHeight, true),
                                         "window", "Game HUD").id;
    // Full-screen page for [nvl].
    const std::string nvl = screen.Card(root, "NVLPanel", Layout(72, 40, 1136, 640),
                                        "#0E1428E6", "#070A14F0", 22).id;
    auto& nvlText = screen.Label(nvl, "RichTextLabel", "NVLText",
                                 Layout(48, 40, 1040, 560), "", 26, kText, "log");
    nvlText.runtimeProperties.emplace("wrap", true);
    // The message window: a glass card along the lower edge with a gold name
    // plate on its upper rim that hides during narration.
    const std::string adv = screen.Card(root, "ADVPanel", Layout(48, 530, 1184, 170),
                                        kDialogueTop, kDialogueBottom, 20).id;
    auto& plate = screen.Box(adv, "NamePlate", Layout(28, -20, 230, 40), kAccent);
    plate.appearance.gradientColor = kAccentDeep;
    plate.appearance.cornerRadius = 20.0f;
    plate.appearance.shadowColor = "#00000066";
    plate.appearance.shadowBlur = 12.0f;
    plate.appearance.shadowOffsetY = 3.0f;
    plate.bindings.emplace("visible", sdk::UiPropertyBinding{"dialogue.hasSpeaker", {}});
    auto& speaker = screen.Label(plate.id, "Label", "Speaker", Layout(0, 0, 230, 40), "", 20,
                                 kInkText, "heading");
    speaker.runtimeProperties.emplace("horizontalAlignment", std::string("Center"));
    speaker.runtimeProperties.emplace("verticalAlignment", std::string("Center"));
    auto& dialogue = screen.Label(adv, "RichTextLabel", "Dialogue",
                                  Layout(44, 40, 1096, 112), "", 26);
    dialogue.runtimeProperties.emplace("wrap", true);
    // Click-wait glyph: shown once the line is complete.
    auto& glyph = screen.Label(adv, "Label", "WaitGlyph", Layout(1140, 136, 24, 22), "▼", 14,
                               kAccent, "status");
    glyph.visible = false;
    glyph.bindings.emplace("visible", sdk::UiPropertyBinding{"dialogue.waiting", {}});
    glyph.runtimeProperties.emplace("showTransitionSeconds", 0.35);
    glyph.runtimeProperties.emplace("showOffsetY", -6.0);
    // Quick menu along the window's top edge.
    const std::pair<const char*, const char*> actions[]{
        {"AUTO", "mode.auto"}, {"SKIP", "mode.skip"}, {"LOG", "backlog.open"},
        {"Q.SAVE", "save.quick"}, {"Q.LOAD", "load.quick"}, {"SAVE", "save.open"},
        {"LOAD", "load.open"}, {"CONFIG", "settings.open"}, {"HIDE", "hud.hide"}};
    const float firstX = 1184.0f - 20.0f - static_cast<float>(std::size(actions)) * 70.0f;
    for (std::size_t index = 0; index < std::size(actions); ++index)
        screen.Quiet(adv, "Quick" + std::to_string(index),
                     Layout(firstX + static_cast<float>(index) * 70.0f, 8, 68, 24),
                     actions[index].first, actions[index].second, 12);
    auto& choices = screen.Node(root, sdk::UiNodeKind::VBox, "VBoxContainer", "Choices",
                                Layout(340, 150, 600, 330), "listbox", "Choices");
    choices.runtimeProperties.emplace("separation", std::int64_t{14});
    screen.Label(root, "Label", "ModeState", Layout(56, 494, 280, 24), "", 15, kAccent);
    // Story [input]: a prompt, a text field and OK, shown while the
    // Player waits for a name.
    auto& input = screen.Card(root, "TextInputPanel", Layout(340, 250, 600, 200));
    input.visible = false;
    input.bindings.emplace("visible", sdk::UiPropertyBinding{"input.visible", {}});
    input.accessibilityRole = "dialog";
    input.accessibilityLabel = "Text input";
    const std::string inputId = input.id;
    auto& prompt = screen.Label(inputId, "Label", "TextInputPrompt",
                                Layout(32, 26, 536, 34), "", 22);
    prompt.bindings.emplace("text", sdk::UiPropertyBinding{"input.prompt", {}});
    auto& field = screen.Node(inputId, sdk::UiNodeKind::Control, "LineEdit", "TextInput",
                              Layout(32, 74, 536, 46), "textbox", "Text");
    field.appearance.backgroundColor = "#FFFFFF12";
    field.appearance.textColor = kText;
    field.appearance.focusColor = kAccent;
    screen.Pill(inputId, "TextInputAccept", Layout(448, 140, 120, 40), "OK",
                "input.accept", 18);
    return screen.Finish();
}

sdk::UiDocument Collection(const int screenNumber, const std::string& documentId,
                           std::string name, std::string eyebrow, std::string heading,
                           std::string runtimeType, std::string listName,
                           const float columns, const sdk::UiVec2Value extent,
                           const sdk::UiVec2Value gap) {
    Screen screen(screenNumber, documentId, "PrismatiX Builtin " + name);
    const std::string root = screen.Backdrop(name + "Root", "dialog", name,
                                             kOverlayTop, kOverlayBottom);
    screen.Heading(root, std::move(eyebrow), std::move(heading));
    screen.Card(root, "ListCard", Layout(56, 128, 1168, 508));
    const auto kind = runtimeType == "ListView" ? sdk::UiNodeKind::Control
                                                : sdk::UiNodeKind::Grid;
    auto& list = screen.Node(root, kind, std::move(runtimeType), std::move(listName),
                             Layout(76, 146, 1128, 472), "list", name + " entries");
    if (list.runtimeType != "ListView")
        list.runtimeProperties.emplace("columns", static_cast<double>(columns));
    list.runtimeProperties.emplace("itemExtent", extent);
    list.runtimeProperties.emplace("gap", gap);
    screen.Pill(root, "Close", Layout(1080, 654, 144, 42), "返回", "overlay.close", 18);
    return screen.Finish();
}

sdk::UiDocument Settings() {
    Screen screen(6, "70000000-0000-4000-8000-000000000006", "PrismatiX Builtin Settings");
    const std::string root = screen.Backdrop("SettingsRoot", "dialog", "Settings",
                                             kOverlayTop, kOverlayBottom);
    screen.Heading(root, "SETTINGS", "設定");
    const std::string sound = screen.Card(root, "SoundCard", Layout(56, 128, 660, 508)).id;
    auto& soundTitle = screen.Label(sound, "Label", "SoundHeading", Layout(32, 24, 400, 22),
                                    "聲音與文字", 15, kAccent);
    soundTitle.runtimeProperties.emplace("letterSpacing", 2.0);
    const std::pair<const char*, const char*> sliders[]{
        {"BGM", "音樂"}, {"SE", "音效"}, {"Voice", "語音"},
        {"TextSpeed", "文字速度"}, {"TextScale", "文字大小"}};
    for (std::size_t index = 0; index < std::size(sliders); ++index) {
        const float y = 70.0f + static_cast<float>(index) * 78.0f;
        screen.Label(sound, "Label", std::string(sliders[index].first) + "Label",
                     Layout(32, y, 160, 36), sliders[index].second, 19, kMuted);
        screen.Node(sound, sdk::UiNodeKind::Control, "Slider", sliders[index].first,
                    Layout(196, y, 420, 36), "slider", sliders[index].second);
    }
    const std::string display = screen.Card(root, "DisplayCard", Layout(736, 128, 488, 508)).id;
    auto& displayTitle = screen.Label(display, "Label", "DisplayHeading", Layout(32, 24, 400, 22),
                                      "顯示與輔助", 15, kAccent);
    displayTitle.runtimeProperties.emplace("letterSpacing", 2.0);
    screen.Label(display, "Label", "LanguageLabel", Layout(32, 64, 200, 36),
                 "語言 / Language", 18, kMuted);
    screen.Node(display, sdk::UiNodeKind::Control, "OptionButton", "Language",
                Layout(228, 60, 228, 44), "combobox", "Language");
    const std::pair<const char*, const char*> checks[]{
        {"SkipRead", "只跳過已讀文字"}, {"Fullscreen", "全螢幕"},
        {"HighContrast", "高對比"}, {"ReducedMotion", "減少動態效果"},
        {"SelfVoicing", "朗讀文字"}};
    for (std::size_t index = 0; index < std::size(checks); ++index) {
        auto& check = screen.Node(display, sdk::UiNodeKind::Button, "CheckBox",
                                  checks[index].first,
                                  Layout(28, 128.0f + static_cast<float>(index) * 62.0f, 428, 48),
                                  "checkbox", checks[index].second);
        check.text = checks[index].second;
        check.runtimeProperties.emplace("fontSize", std::int64_t{19});
    }
    screen.Pill(root, "Close", Layout(1080, 654, 144, 42), "完成", "overlay.close", 18);
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
        {"backlog", Collection(3, "70000000-0000-4000-8000-000000000003", "Backlog",
                               "BACKLOG", "履歷", "ListView", "Entries", 1,
                               {1128, 64}, {0, 8})},
        {"save", Collection(4, "70000000-0000-4000-8000-000000000004", "Save", "SAVE",
                            "存檔", "GridView", "Slots", 3, {366, 104}, {15, 14})},
        {"load", Collection(8, "70000000-0000-4000-8000-000000000008", "Load", "LOAD",
                            "讀取", "GridView", "Slots", 3, {366, 104}, {15, 14})},
        {"gallery", Collection(5, "70000000-0000-4000-8000-000000000005", "Gallery",
                               "GALLERY", "CG 鑑賞", "GridView", "Items", 4,
                               {270, 160}, {16, 16})},
        {"settings", Settings()},
        {"video", Video()},
    };
    if (width > 0 && height > 0 &&
        (width != static_cast<int>(kDesignWidth) || height != static_cast<int>(kDesignHeight)))
        for (auto& entry : entries) ScaleDocument(entry.document, width, height);
    return entries;
}

}  // namespace px::ui
