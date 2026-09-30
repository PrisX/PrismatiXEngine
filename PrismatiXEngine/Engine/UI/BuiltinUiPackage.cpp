#include "Engine/UI/BuiltinUiPackage.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace px::ui {
namespace {

// Screens are designed on a 1280x720 canvas and scaled to the project's
// logical resolution when they are installed.
constexpr float kDesignWidth = 1280.0f;
constexpr float kDesignHeight = 720.0f;

// One restrained palette for every stock screen: ink panels, warm off-white
// text, and a single gold accent for names, headings and focus.
constexpr char kScreen[] = "#0B0E14F5";
constexpr char kTitleScreen[] = "#0E1117FF";
constexpr char kDialoguePanel[] = "#0D1119E0";
constexpr char kNvlPanel[] = "#0D1119EB";
constexpr char kText[] = "#EEF0F4FF";
constexpr char kMuted[] = "#A3ABB9FF";
constexpr char kAccent[] = "#E6C27AFF";
constexpr char kButton[] = "#FFFFFF12";
constexpr char kButtonHover[] = "#E6C27A33";
constexpr char kQuiet[] = "#00000000";
constexpr char kQuietHover[] = "#FFFFFF1F";

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

sdk::UiNode Node(std::string id, std::optional<std::string> parent,
                 const std::uint32_t order, const sdk::UiNodeKind kind,
                 std::string runtimeType, std::string name,
                 const sdk::UiLayout& layout, std::string role,
                 std::string label) {
    sdk::UiNode node;
    node.id = std::move(id);
    node.parentId = std::move(parent);
    node.order = order;
    node.kind = kind;
    if (!runtimeType.empty()) node.runtimeType = std::move(runtimeType);
    node.name = std::move(name);
    node.layout = layout;
    node.accessibilityRole = std::move(role);
    node.accessibilityLabel = std::move(label);
    node.accessibilityFocusOrder = static_cast<std::int32_t>(order);
    return node;
}

// A flat colour block: a panel background, a scrim or an accent rule.
sdk::UiNode Fill(std::string id, std::optional<std::string> parent,
                 const std::uint32_t order, std::string name,
                 const sdk::UiLayout& layout, const char* color) {
    auto node = Node(std::move(id), std::move(parent), order,
                     sdk::UiNodeKind::Group, {}, std::move(name), layout,
                     "group", {});
    node.appearance.backgroundColor = color;
    return node;
}

sdk::UiNode Label(std::string id, std::string parent, const std::uint32_t order,
                  std::string type, std::string name, const sdk::UiLayout& layout,
                  std::string text, const std::int64_t fontSize,
                  const char* color = kText, std::string role = "text") {
    auto node = Node(std::move(id), std::move(parent), order,
                     sdk::UiNodeKind::Label, std::move(type), std::move(name),
                     layout, std::move(role), text);
    node.text = std::move(text);
    node.appearance.textColor = color;
    node.runtimeProperties.emplace("fontSize", fontSize);
    return node;
}

sdk::UiNode Button(std::string id, std::string parent,
                   const std::uint32_t order, std::string name,
                   const sdk::UiLayout& layout, std::string text,
                   std::string action, const std::int64_t fontSize = 20,
                   const bool quiet = false) {
    auto node = Node(std::move(id), std::move(parent), order,
                     sdk::UiNodeKind::Button, {}, std::move(name),
                     layout, "button", text);
    node.text = std::move(text);
    node.onClick = sdk::UiAction{.id = std::move(action)};
    node.appearance.backgroundColor = quiet ? kQuiet : kButton;
    node.appearance.hoverBackgroundColor = quiet ? kQuietHover : kButtonHover;
    node.appearance.textColor = quiet ? "#C9CFDAFF" : kText;
    node.appearance.focusColor = kAccent;
    node.runtimeProperties.emplace("focusMode", std::string("All"));
    node.runtimeProperties.emplace("fontSize", fontSize);
    return node;
}

sdk::UiDocument Document(std::string id, std::string name,
                         std::vector<sdk::UiNode> nodes) {
    sdk::UiDocument document;
    document.id = std::move(id);
    document.revision = 1;
    document.name = std::move(name);
    document.width = static_cast<int>(kDesignWidth);
    document.height = static_cast<int>(kDesignHeight);
    document.rootId = nodes.front().id;
    document.nodes = std::move(nodes);
    document.theme = {
        {"dialogue.background", "Dialogue background", kDialoguePanel},
        {"focus.color", "Focus color", kAccent}};
    return document;
}

// Screen heading with the accent rule every stock menu shares.
void Heading(std::vector<sdk::UiNode>& nodes, const std::string& root,
             const std::string& prefix, std::string text) {
    nodes.push_back(Label(prefix + "4", root, 2, "Label", "ScreenHeading",
                          Layout(72, 40, 800, 48), std::move(text), 34,
                          "#F4F1EAFF", "heading"));
    nodes.push_back(Fill(prefix + "5", root, 3, "HeadingRule",
                         Layout(72, 92, 48, 3), kAccent));
}

sdk::UiDocument Title() {
    constexpr char root[] = "71000000-0000-4000-8000-000000000001";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Fill(root, std::nullopt, 0, "TitleRoot",
                         Layout(0, 0, kDesignWidth, kDesignHeight, true),
                         kTitleScreen));
    nodes.back().accessibilityRole = "window";
    nodes.back().accessibilityLabel = "Game title";
    nodes.push_back(Label("71000000-0000-4000-8000-000000000002", root, 0,
                          "Label", "GameTitle", Layout(96, 176, 1000, 84),
                          "PrismatiX", 60, "#F4F1EAFF", "heading"));
    nodes.push_back(Fill("71000000-0000-4000-8000-000000000007", root, 1,
                         "TitleRule", Layout(98, 268, 56, 3), kAccent));
    const std::pair<const char*, const char*> entries[]{
        {"開始遊戲", "game.start"}, {"讀取進度", "load.open"},
        {"CG 鑑賞", "gallery.open"}, {"設定", "settings.open"},
        {"結束", "app.quit"}};
    for (std::uint32_t index = 0; index < std::size(entries); ++index)
        nodes.push_back(Button(
            "71000000-0000-4000-8000-00000000001" + std::to_string(index), root,
            2 + index, index == 0 ? "Start" : index == 1 ? "Load"
                       : index == 2 ? "Gallery" : index == 3 ? "Settings" : "Quit",
            Layout(96, 304.0f + index * 58.0f, 260, 48), entries[index].first,
            entries[index].second, 22));
    nodes.push_back(Label("71000000-0000-4000-8000-000000000008", root, 9,
                          "Label", "Credit", Layout(96, 664, 400, 24),
                          "Made with PrismatiX", 14, kMuted));
    return Document("70000000-0000-4000-8000-000000000001",
                    "PrismatiX Builtin Title", std::move(nodes));
}

sdk::UiDocument Hud() {
    constexpr char root[] = "72000000-0000-4000-8000-000000000001";
    constexpr char nvl[] = "72000000-0000-4000-8000-000000000002";
    constexpr char adv[] = "72000000-0000-4000-8000-000000000004";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(root, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Container", "HUDRoot",
                         Layout(0, 0, kDesignWidth, kDesignHeight, true),
                         "window", "Game HUD"));
    nodes.push_back(Node(nvl, root, 0, sdk::UiNodeKind::Group, "Panel",
                         "NVLPanel", Layout(80, 48, 1120, 600), "group",
                         "NVL panel"));
    nodes.back().appearance.backgroundColor = kNvlPanel;
    auto nvlText = Label("72000000-0000-4000-8000-000000000003", nvl, 0,
                         "RichTextLabel", "NVLText",
                         Layout(40, 36, 1040, 528), "", 26, kText, "log");
    nvlText.runtimeProperties.emplace("wrap", true);
    nodes.push_back(std::move(nvlText));
    // The message box spans the lower edge; the speaker sits on its first
    // line in the accent colour, the way most commercial ADV layouts do.
    nodes.push_back(Node(adv, root, 1, sdk::UiNodeKind::Group, "Panel",
                         "ADVPanel", Layout(40, 520, 1200, 180), "group",
                         "Dialogue panel"));
    nodes.back().appearance.backgroundColor = kDialoguePanel;
    nodes.push_back(Label("72000000-0000-4000-8000-000000000005", adv, 0,
                          "Label", "Speaker", Layout(32, 16, 600, 34), "", 24,
                          kAccent, "heading"));
    auto dialogue = Label("72000000-0000-4000-8000-000000000006", adv, 1,
                          "RichTextLabel", "Dialogue",
                          Layout(32, 56, 1136, 112), "", 26);
    dialogue.runtimeProperties.emplace("wrap", true);
    nodes.push_back(std::move(dialogue));
    auto choices = Node("72000000-0000-4000-8000-000000000007", root, 2,
                        sdk::UiNodeKind::VBox, "VBoxContainer", "Choices",
                        Layout(340, 140, 600, 330), "listbox", "Choices");
    choices.runtimeProperties.emplace("separation", std::int64_t{14});
    nodes.push_back(std::move(choices));
    nodes.push_back(Label("72000000-0000-4000-8000-000000000008", root, 3,
                          "Label", "ModeState", Layout(48, 486, 240, 28), "",
                          16, kAccent));
    const std::pair<const char*, const char*> actions[]{
        {"AUTO", "mode.auto"}, {"SKIP", "mode.skip"},
        {"LOG", "backlog.open"}, {"SAVE", "save.open"},
        {"LOAD", "load.open"}, {"CONFIG", "settings.open"}};
    for (std::uint32_t index = 0; index < std::size(actions); ++index)
        nodes.push_back(Button(
            "72000000-0000-4000-8000-0000000000" + std::to_string(10 + index),
            root, 10 + index, "Quick" + std::to_string(index),
            Layout(744.0f + index * 80.0f, 530, 76, 28), actions[index].first,
            actions[index].second, 15, true));
    return Document("70000000-0000-4000-8000-000000000002",
                    "PrismatiX Builtin HUD", std::move(nodes));
}

sdk::UiDocument Collection(const std::string& documentId,
                           const std::string& rootId, std::string name,
                           std::string heading, std::string runtimeType,
                           std::string listName, const float columns,
                           const sdk::UiVec2Value extent, const sdk::UiVec2Value gap) {
    std::vector<sdk::UiNode> nodes;
    const std::string prefix = rootId.substr(0, rootId.size() - 1);
    nodes.push_back(Fill(rootId, std::nullopt, 0, name + "Root",
                         Layout(0, 0, kDesignWidth, kDesignHeight, true), kScreen));
    nodes.back().accessibilityRole = "dialog";
    nodes.back().accessibilityLabel = name;
    const auto kind = runtimeType == "ListView" ? sdk::UiNodeKind::Control
                                                : sdk::UiNodeKind::Grid;
    auto list = Node(prefix + "2", rootId, 0, kind, std::move(runtimeType),
                     std::move(listName), Layout(72, 116, 1136, 504), "list",
                     name + " entries");
    if (list.runtimeType != "ListView")
        list.runtimeProperties.emplace("columns", static_cast<double>(columns));
    list.runtimeProperties.emplace("itemExtent", extent);
    list.runtimeProperties.emplace("gap", gap);
    nodes.push_back(std::move(list));
    Heading(nodes, rootId, prefix, std::move(heading));
    nodes.push_back(Button(prefix + "3", rootId, 1, "Close",
                           Layout(1088, 644, 120, 42), "返回",
                           "overlay.close", 18));
    return Document(documentId, "PrismatiX Builtin " + name, std::move(nodes));
}

sdk::UiDocument Settings() {
    constexpr char root[] = "76000000-0000-4000-8000-000000000001";
    const std::string prefix = "76000000-0000-4000-8000-00000000006";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Fill(root, std::nullopt, 0, "SettingsRoot",
                         Layout(0, 0, kDesignWidth, kDesignHeight, true), kScreen));
    nodes.back().accessibilityRole = "dialog";
    nodes.back().accessibilityLabel = "Settings";
    Heading(nodes, root, prefix, "設定");
    const std::pair<const char*, const char*> sliders[]{
        {"BGM", "音樂"}, {"SE", "音效"}, {"Voice", "語音"},
        {"TextSpeed", "文字速度"}, {"TextScale", "文字大小"}};
    for (std::uint32_t index = 0; index < std::size(sliders); ++index) {
        const float y = 132.0f + index * 56.0f;
        nodes.push_back(Label(
            "76000000-0000-4000-8000-0000000000" + std::to_string(40 + index),
            root, 40 + index, "Label", std::string(sliders[index].first) + "Label",
            Layout(96, y, 180, 36), sliders[index].second, 20, kMuted));
        nodes.push_back(Node(
            "76000000-0000-4000-8000-0000000000" + std::to_string(10 + index),
            // Orders 2 and 3 belong to the heading.
            root, 4 + index, sdk::UiNodeKind::Control, "Slider",
            sliders[index].first, Layout(300, y, 520, 36), "slider",
            sliders[index].second));
    }
    const std::pair<const char*, const char*> checks[]{
        {"SkipRead", "只跳過已讀文字"}, {"Fullscreen", "全螢幕"},
        {"HighContrast", "高對比"}, {"ReducedMotion", "減少動態效果"},
        {"SelfVoicing", "朗讀文字"}};
    for (std::uint32_t index = 0; index < std::size(checks); ++index) {
        auto check = Node(
            "76000000-0000-4000-8000-0000000000" + std::to_string(20 + index),
            root, 10 + index, sdk::UiNodeKind::Button, "CheckBox",
            checks[index].first,
            Layout(96.0f + (index % 2) * 380.0f, 430.0f + (index / 2) * 54.0f,
                   350, 44),
            "checkbox", checks[index].second);
        check.text = checks[index].second;
        check.runtimeProperties.emplace("fontSize", std::int64_t{20});
        nodes.push_back(std::move(check));
    }
    nodes.push_back(Label("76000000-0000-4000-8000-000000000033", root, 29,
                          "Label", "LanguageLabel", Layout(900, 132, 260, 36),
                          "語言 / Language", 20, kMuted));
    nodes.push_back(Node("76000000-0000-4000-8000-000000000030", root, 30,
                         sdk::UiNodeKind::Control, "OptionButton", "Language",
                         Layout(900, 176, 260, 44), "combobox", "Language"));
    nodes.push_back(Button("76000000-0000-4000-8000-000000000031", root, 31,
                           "Close", Layout(1088, 644, 120, 42), "完成",
                           "overlay.close", 18));
    return Document("70000000-0000-4000-8000-000000000006",
                    "PrismatiX Builtin Settings", std::move(nodes));
}

sdk::UiDocument Video() {
    constexpr char root[] = "77000000-0000-4000-8000-000000000001";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(root, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Container", "VideoRoot",
                         Layout(0, 0, kDesignWidth, kDesignHeight, true),
                         "window", "Video overlay"));
    nodes.push_back(Label("77000000-0000-4000-8000-000000000002", root, 0,
                          "Label", "SkipHint", Layout(960, 660, 280, 40), "",
                          16, kMuted));
    return Document("70000000-0000-4000-8000-000000000007",
                    "PrismatiX Builtin Video", std::move(nodes));
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
        for (auto& [name, value] : node.runtimeProperties) {
            if (name == "fontSize" || name == "separation") {
                if (auto* integer = std::get_if<std::int64_t>(&value))
                    *integer = std::max<std::int64_t>(
                        1, std::llround(static_cast<double>(*integer) * sf));
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
        {"backlog", Collection(
                        "70000000-0000-4000-8000-000000000003",
                        "73000000-0000-4000-8000-000000000001", "Backlog",
                        "履歷", "ListView", "Entries", 1, {1136, 60}, {0, 6})},
        {"save", Collection(
                     "70000000-0000-4000-8000-000000000004",
                     "74000000-0000-4000-8000-000000000001", "Save", "存檔",
                     "GridView", "Slots", 3, {370, 96}, {13, 12})},
        {"load", Collection(
                     "70000000-0000-4000-8000-000000000008",
                     "78000000-0000-4000-8000-000000000001", "Load", "讀取",
                     "GridView", "Slots", 3, {370, 96}, {13, 12})},
        {"gallery", Collection(
                        "70000000-0000-4000-8000-000000000005",
                        "75000000-0000-4000-8000-000000000001", "Gallery",
                        "CG 鑑賞", "GridView", "Items", 4, {275, 168}, {12, 12})},
        {"settings", Settings()},
        {"video", Video()},
    };
    if (width > 0 && height > 0 &&
        (width != static_cast<int>(kDesignWidth) || height != static_cast<int>(kDesignHeight)))
        for (auto& entry : entries) ScaleDocument(entry.document, width, height);
    return entries;
}

}  // namespace px::ui
