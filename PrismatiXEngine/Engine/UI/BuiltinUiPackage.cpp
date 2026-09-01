#include "Engine/UI/BuiltinUiPackage.h"

#include <optional>
#include <string>
#include <utility>

namespace px::ui {
namespace {

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
    node.runtimeType = std::move(runtimeType);
    node.name = std::move(name);
    node.layout = layout;
    node.accessibilityRole = std::move(role);
    node.accessibilityLabel = std::move(label);
    node.accessibilityFocusOrder = static_cast<std::int32_t>(order);
    return node;
}

sdk::UiNode Label(std::string id, std::string parent, const std::uint32_t order,
                  std::string type, std::string name, const sdk::UiLayout& layout,
                  std::string text, std::string role = "text") {
    auto node = Node(std::move(id), std::move(parent), order,
                     sdk::UiNodeKind::Label, std::move(type), std::move(name),
                     layout, std::move(role), text);
    node.text = std::move(text);
    return node;
}

sdk::UiNode Button(std::string id, std::string parent,
                   const std::uint32_t order, std::string name,
                   const sdk::UiLayout& layout, std::string text,
                   std::string action) {
    auto node = Node(std::move(id), std::move(parent), order,
                     sdk::UiNodeKind::Button, "Button", std::move(name),
                     layout, "button", text);
    node.text = std::move(text);
    node.onClick = sdk::UiAction{.id = std::move(action)};
    node.runtimeProperties.emplace("focusMode", std::string("All"));
    return node;
}

sdk::UiDocument Document(std::string id, std::string name,
                         std::vector<sdk::UiNode> nodes) {
    sdk::UiDocument document;
    document.id = std::move(id);
    document.revision = 1;
    document.name = std::move(name);
    document.width = 1280;
    document.height = 720;
    document.rootId = nodes.front().id;
    document.nodes = std::move(nodes);
    document.theme = {
        {"dialogue.background", "Dialogue background", "#D0182038"},
        {"focus.color", "Focus color", "#FFF0C060"}};
    return document;
}

sdk::UiDocument Title() {
    constexpr char root[] = "71000000-0000-4000-8000-000000000001";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(root, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Panel", "TitleRoot", Layout(0, 0, 1280, 720, true),
                         "window", "Game title"));
    auto title = Label("71000000-0000-4000-8000-000000000002", root, 0,
                       "Label", "GameTitle", Layout(110, 80, 720, 100),
                       "PrismatiX", "heading");
    title.runtimeProperties.emplace("fontSize", std::int64_t{52});
    nodes.push_back(std::move(title));
    nodes.push_back(Button("71000000-0000-4000-8000-000000000003", root, 1,
                           "Start", Layout(110, 300, 300, 64), "開始遊戲",
                           "game.start"));
    nodes.push_back(Button("71000000-0000-4000-8000-000000000004", root, 2,
                           "Load", Layout(110, 380, 300, 64), "讀取遊戲",
                           "load.open"));
    nodes.push_back(Button("71000000-0000-4000-8000-000000000005", root, 3,
                           "Gallery", Layout(110, 460, 300, 64), "CG 鑑賞",
                           "gallery.open"));
    nodes.push_back(Button("71000000-0000-4000-8000-000000000006", root, 4,
                           "Settings", Layout(110, 540, 300, 64), "設定",
                           "settings.open"));
    return Document("70000000-0000-4000-8000-000000000001",
                    "PrismatiX Builtin Title", std::move(nodes));
}

sdk::UiDocument Hud() {
    constexpr char root[] = "72000000-0000-4000-8000-000000000001";
    constexpr char nvl[] = "72000000-0000-4000-8000-000000000002";
    constexpr char adv[] = "72000000-0000-4000-8000-000000000004";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(root, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Container", "HUDRoot", Layout(0, 0, 1280, 720, true),
                         "window", "Game HUD"));
    nodes.push_back(Node(nvl, root, 0, sdk::UiNodeKind::Group, "Panel",
                         "NVLPanel", Layout(60, 40, 1160, 570), "group",
                         "NVL panel"));
    auto nvlText = Label("72000000-0000-4000-8000-000000000003", nvl, 0,
                         "RichTextLabel", "NVLText",
                         Layout(30, 30, 1100, 510), "", "log");
    nvlText.runtimeProperties.emplace("wrap", true);
    nvlText.runtimeProperties.emplace("fontSize", std::int64_t{26});
    nodes.push_back(std::move(nvlText));
    nodes.push_back(Node(adv, root, 1, sdk::UiNodeKind::Group, "Panel",
                         "ADVPanel", Layout(55, 460, 1170, 220), "group",
                         "Dialogue panel"));
    auto speaker = Label("72000000-0000-4000-8000-000000000005", adv, 0,
                         "Label", "Speaker", Layout(30, 18, 1040, 38), "",
                         "heading");
    speaker.runtimeProperties.emplace("fontSize", std::int64_t{25});
    nodes.push_back(std::move(speaker));
    auto dialogue = Label("72000000-0000-4000-8000-000000000006", adv, 1,
                          "RichTextLabel", "Dialogue",
                          Layout(30, 62, 1100, 130), "");
    dialogue.runtimeProperties.emplace("wrap", true);
    dialogue.runtimeProperties.emplace("fontSize", std::int64_t{30});
    nodes.push_back(std::move(dialogue));
    auto choices = Node("72000000-0000-4000-8000-000000000007", root, 2,
                        sdk::UiNodeKind::VBox, "VBoxContainer", "Choices",
                        Layout(300, 145, 680, 280), "listbox", "Choices");
    choices.runtimeProperties.emplace("separation", std::int64_t{12});
    nodes.push_back(std::move(choices));
    nodes.push_back(Label("72000000-0000-4000-8000-000000000008", root, 3,
                          "Label", "ModeState", Layout(25, 20, 300, 34), ""));
    const std::pair<const char*, const char*> actions[]{
        {"AUTO", "mode.auto"}, {"SKIP", "mode.skip"},
        {"LOG", "backlog.open"}, {"SAVE", "save.open"},
        {"LOAD", "load.open"}, {"⚙", "settings.open"}};
    for (std::uint32_t index = 0; index < std::size(actions); ++index)
        nodes.push_back(Button(
            "72000000-0000-4000-8000-0000000000" +
                std::to_string(10 + index),
            root, 10 + index, "Quick" + std::to_string(index),
            Layout(650.0f + index * 96.0f, 18, 88, 42), actions[index].first,
            actions[index].second));
    return Document("70000000-0000-4000-8000-000000000002",
                    "PrismatiX Builtin HUD", std::move(nodes));
}

sdk::UiDocument Collection(const std::string& documentId,
                           const std::string& rootId, std::string name,
                           std::string runtimeType, std::string listName) {
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(rootId, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Panel", name + "Root", Layout(0, 0, 1280, 720, true),
                         "dialog", name));
    const std::string listId = rootId.substr(0, rootId.size() - 1) + "2";
    const auto kind = runtimeType == "ListView" ? sdk::UiNodeKind::Control
                                                : sdk::UiNodeKind::Grid;
    nodes.push_back(Node(listId, rootId, 0, kind, std::move(runtimeType),
                         std::move(listName), Layout(50, 60, 1180, 570),
                         "list", name + " entries"));
    nodes.push_back(Button(rootId.substr(0, rootId.size() - 1) + "3", rootId,
                           1, "Close", Layout(1040, 642, 180, 52), "返回",
                           "overlay.close"));
    return Document(documentId, "PrismatiX Builtin " + name,
                    std::move(nodes));
}

sdk::UiDocument Settings() {
    constexpr char root[] = "76000000-0000-4000-8000-000000000001";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(root, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Panel", "SettingsRoot", Layout(0, 0, 1280, 720, true),
                         "dialog", "Settings"));
    const std::pair<const char*, const char*> sliders[]{
        {"BGM", "BGM volume"}, {"SE", "Sound effect volume"},
        {"Voice", "Voice volume"}, {"TextSpeed", "Text speed"},
        {"TextScale", "Text scale"}};
    for (std::uint32_t index = 0; index < std::size(sliders); ++index) {
        auto node = Node(
            "76000000-0000-4000-8000-0000000000" +
                std::to_string(10 + index),
            root, index, sdk::UiNodeKind::Control, "Slider",
            sliders[index].first, Layout(250, 90.0f + index * 76.0f, 760, 40),
            "slider", sliders[index].second);
        nodes.push_back(std::move(node));
    }
    const std::pair<const char*, const char*> checks[]{
        {"SkipRead", "Skip read text only"}, {"Fullscreen", "Fullscreen"},
        {"HighContrast", "High contrast"},
        {"ReducedMotion", "Reduced motion"},
        {"SelfVoicing", "Self voicing"}};
    for (std::uint32_t index = 0; index < std::size(checks); ++index)
        nodes.push_back(Node(
            "76000000-0000-4000-8000-0000000000" +
                std::to_string(20 + index),
            root, 10 + index, sdk::UiNodeKind::Button, "CheckBox",
            checks[index].first,
            Layout(220.0f + (index % 2) * 430.0f,
                   480.0f + (index / 2) * 58.0f, 390, 48),
            "checkbox", checks[index].second));
    nodes.push_back(Node("76000000-0000-4000-8000-000000000030", root, 30,
                         sdk::UiNodeKind::Control, "OptionButton", "Language",
                         Layout(850, 480, 260, 48), "combobox", "Language"));
    nodes.push_back(Button("76000000-0000-4000-8000-000000000031", root, 31,
                           "Close", Layout(1020, 642, 180, 52), "完成",
                           "overlay.close"));
    return Document("70000000-0000-4000-8000-000000000006",
                    "PrismatiX Builtin Settings", std::move(nodes));
}

sdk::UiDocument Video() {
    constexpr char root[] = "77000000-0000-4000-8000-000000000001";
    std::vector<sdk::UiNode> nodes;
    nodes.push_back(Node(root, std::nullopt, 0, sdk::UiNodeKind::Group,
                         "Container", "VideoRoot", Layout(0, 0, 1280, 720, true),
                         "window", "Video overlay"));
    nodes.push_back(Label("77000000-0000-4000-8000-000000000002", root, 0,
                          "Label", "SkipHint", Layout(960, 640, 280, 48), ""));
    return Document("70000000-0000-4000-8000-000000000007",
                    "PrismatiX Builtin Video", std::move(nodes));
}

}  // namespace

std::vector<BuiltinUiEntry> CreateBuiltinUiPackage() {
    return {
        {"title", Title()},
        {"hud", Hud()},
        {"backlog", Collection(
                        "70000000-0000-4000-8000-000000000003",
                        "73000000-0000-4000-8000-000000000001", "Backlog",
                        "ListView", "Entries")},
        {"save", Collection(
                     "70000000-0000-4000-8000-000000000004",
                     "74000000-0000-4000-8000-000000000001", "Save",
                     "GridView", "Slots")},
        {"load", Collection(
                     "70000000-0000-4000-8000-000000000008",
                     "78000000-0000-4000-8000-000000000001", "Load",
                     "GridView", "Slots")},
        {"gallery", Collection(
                        "70000000-0000-4000-8000-000000000005",
                        "75000000-0000-4000-8000-000000000001", "Gallery",
                        "GridView", "Items")},
        {"settings", Settings()},
        {"video", Video()},
    };
}

}  // namespace px::ui
