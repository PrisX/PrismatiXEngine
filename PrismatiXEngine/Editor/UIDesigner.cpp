#define IMGUI_DEFINE_MATH_OPERATORS

#include "UIDesigner.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

namespace {

constexpr const char* kResourcePayload = "PX_RESOURCE_PATH";
constexpr float kMinNodeSize = 8.0f;

std::string Trim(std::string_view value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string RuntimeTypeFromAsset(const std::string& runtimePath) {
    std::string ext = fs::path(runtimePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".webp") {
        return "image";
    }
    return "button";
}

float Snap(float value, float grid) {
    if (grid <= 0.0f) {
        return value;
    }
    return std::round(value / grid) * grid;
}

bool StringContains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    std::string a(haystack);
    std::string b(needle);
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return a.find(b) != std::string::npos;
}

}

UIDesigner::UIDesigner(LogSink log) : m_log(std::move(log)) {
    SeedDefaultScene();
    m_dirty = false;
}

void UIDesigner::SetProject(const ProjectContext* context) {
    if (m_project == context && context && !DocumentPath().empty()) {
        return;
    }
    m_project = context;
    LoadOrCreate();
}

void UIDesigner::SetSelectedResourceCallback(SelectedResourceCallback callback) {
    m_selectedResource = std::move(callback);
}

void UIDesigner::SetTextureResolver(TextureResolver resolver) {
    m_textureResolver = std::move(resolver);
}

void UIDesigner::SetRuntimePreviewCallback(RuntimePreviewCallback callback) {
    m_runtimePreview = std::move(callback);
}

fs::path UIDesigner::DocumentPath() const {
    if (!m_project || !m_project->IsOpen()) {
        return {};
    }
    return m_project->DataRoot() / fs::path(CurrentDocumentRuntimePath());
}

std::string UIDesigner::CurrentDocumentRuntimePath() const {
    return m_documentRuntimePath.empty() ? "UI/title_menu.pxui" : m_documentRuntimePath;
}

std::string UIDesigner::GeneratedSceneScriptPath() const {
    return GeneratedScriptPathForRuntimePath(CurrentDocumentRuntimePath());
}

void UIDesigner::LoadOrCreate() {
    ClearSelection();
    if (!m_project || !m_project->IsOpen()) {
        SeedDefaultScene();
        m_dirty = false;
        return;
    }

    const fs::path path = DocumentPath();
    if (fs::exists(path)) {
        try {
            std::ifstream in(path, std::ios::binary);
            const Json document = Json::parse(in);
            LoadDocument(document);
            m_dirty = false;
            Log("Loaded UI document: " + path.string());
            return;
        } catch (const std::exception& exception) {
            Log("UI document parse failed, seeded a clean title UI: " + std::string(exception.what()));
        }
    }

    SeedTemplateDocument(CurrentDocumentRuntimePath());
    Save();
}

void UIDesigner::SeedDefaultScene() {
    m_layers.clear();
    m_nodes.clear();
    m_selectedIds.clear();
    m_canvasW = 1280;
    m_canvasH = 720;
    m_backgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    m_backgroundImage = "Image/Background/title_bg.jpg";
    m_nextNodeIndex = 1;
    m_zoom = 0.70f;
    m_pan = ImVec2(0, 0);

    m_layers.push_back({ "background", "Background", true, false, true });
    m_layers.push_back({ "sidebar", "Main Menu Sidebar", true, false, true });
    m_layers.push_back({ "menu", "Image Buttons", true, false, true });

    auto makeNode = [&](std::string id, std::string type, std::string name, std::string layer, int order, float x, float y, float w, float h) -> Node& {
        Node node;
        node.id = std::move(id);
        node.type = std::move(type);
        node.name = std::move(name);
        node.layerId = std::move(layer);
        node.order = order;
        node.x = x;
        node.y = y;
        node.w = w;
        node.h = h;
        node.layer = static_cast<int>(std::distance(m_layers.begin(), std::find_if(m_layers.begin(), m_layers.end(), [&](const Layer& candidate) {
            return candidate.id == node.layerId;
        })));
        m_nodes.push_back(std::move(node));
        return m_nodes.back();
    };

    Node& sidebar = makeNode("main_menu_sidebar", "image", "Main Menu Sidebar", "sidebar", 0, 0.0f, 0.0f, 939.0f, 720.0f);
    sidebar.image = "Image/UI/Main_Menu/Main menu sidebar.png";
    sidebar.bgColor = ImVec4(0, 0, 0, 0);
    sidebar.hoverColor = sidebar.bgColor;
    sidebar.borderColor = ImVec4(0, 0, 0, 0);
    sidebar.hoverBorderColor = sidebar.borderColor;
    sidebar.actionType = "ui.toggle";
    sidebar.actionTarget.clear();

    auto makeButton = [&](std::string id, std::string name, int order, float x, float y, float w, float h, std::string image, std::string hoverImage, std::string action, std::string target = {}) -> Node& {
        Node& button = makeNode(std::move(id), "button", std::move(name), "menu", order, x, y, w, h);
        button.text.clear();
        button.image = "Image/UI/Main_Menu/" + image;
        button.hoverImage = "Image/UI/Main_Menu/" + hoverImage;
        button.bgColor = ImVec4(0, 0, 0, 0);
        button.hoverColor = ImVec4(0, 0, 0, 0);
        button.borderColor = ImVec4(0, 0, 0, 0);
        button.hoverBorderColor = ImVec4(0, 0, 0, 0);
        button.radius = 0.0f;
        button.actionType = std::move(action);
        button.actionTarget = std::move(target);
        return button;
    };

    makeButton("continue_button", "Continue", 0, 76.0f, 270.0f, 250.0f, 59.0f, "Continue.png", "Continue_hovered.png", "save.open", "continue");
    Node& start = makeButton("new_button", "New Game", 1, 78.0f, 328.0f, 143.0f, 60.0f, "New.png", "New_hovered.png", "scene.switch", "Scripts/scenes/play_scene.lua");
    start.animations.push_back({ "Intro Slide", 0.35f, 0.04f, "outCubic", false, "scene.enter", { { 0.0f, "x", 42.0f }, { 0.35f, "x", 78.0f } } });
    makeButton("load_button", "Load", 2, 80.0f, 386.0f, 154.0f, 58.0f, "Load.png", "Load_hovered.png", "save.open", "save_menu");
    makeButton("config_button", "Config", 3, 80.0f, 444.0f, 205.0f, 58.0f, "Config.png", "Config_hovered.png", "ui.toggle", "config_menu");
    makeButton("gallery_button", "Gallery", 4, 80.0f, 502.0f, 219.0f, 58.0f, "Gallery.png", "Gallery_hovered.png", "ui.toggle", "gallery_menu");
    makeButton("exit_button", "Exit", 5, 80.0f, 560.0f, 132.0f, 58.0f, "Exit.png", "Exit_hovered.png", "game.exit");

    m_revision++;
}

void UIDesigner::SeedBlankDocument() {
    m_layers.clear();
    m_nodes.clear();
    m_selectedIds.clear();
    m_canvasW = 1280;
    m_canvasH = 720;
    m_backgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    m_backgroundImage.clear();
    m_nextNodeIndex = 1;
    m_zoom = 0.72f;
    m_pan = ImVec2(0, 0);
    m_layers.push_back({ "background", "Background", true, false, true });
    m_layers.push_back({ "content", "Content", true, false, true });
    m_layers.push_back({ "hud", "HUD", true, false, true });
    m_revision++;
}

void UIDesigner::SeedTemplateDocument(const std::string& runtimePath) {
    std::string lower = runtimePath;
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.ends_with("title_menu.pxui")) {
        SeedDefaultScene();
        return;
    }

    SeedBlankDocument();
    m_layers.clear();
    m_nodes.clear();
    m_selectedIds.clear();
    m_backgroundColor = ImVec4(0, 0, 0, 0.86f);
    m_backgroundImage.clear();
    m_layers.push_back({ "background", "Background", true, false, true });
    m_layers.push_back({ "content", "Content", true, false, true });
    m_layers.push_back({ "controls", "Controls", true, false, true });

    auto makeNode = [&](std::string id, std::string type, std::string name, std::string layer, int order, float x, float y, float w, float h) -> Node& {
        Node node;
        node.id = std::move(id);
        node.type = std::move(type);
        node.name = std::move(name);
        node.layerId = std::move(layer);
        node.order = order;
        node.x = x;
        node.y = y;
        node.w = w;
        node.h = h;
        node.layer = static_cast<int>(std::distance(m_layers.begin(), std::find_if(m_layers.begin(), m_layers.end(), [&](const Layer& candidate) { return candidate.id == node.layerId; })));
        m_nodes.push_back(std::move(node));
        return m_nodes.back();
    };
    auto panelStyle = [](Node& node, ImVec4 color = ImVec4(0.05f, 0.07f, 0.10f, 0.86f)) {
        node.bgColor = color;
        node.hoverColor = color;
        node.borderColor = ImVec4(0.30f, 0.42f, 0.55f, 0.45f);
        node.hoverBorderColor = ImVec4(0.45f, 0.95f, 0.82f, 0.80f);
        node.radius = 8.0f;
        node.actionType = "ui.toggle";
        node.actionTarget.clear();
    };
    auto textStyle = [](Node& node, int size = 28) {
        node.type = "text";
        node.bgColor = ImVec4(0, 0, 0, 0);
        node.hoverColor = node.bgColor;
        node.textColor = ImVec4(0.92f, 0.97f, 1.0f, 1.0f);
        node.fontSize = size;
        node.textShadow = true;
        node.actionType = "ui.toggle";
        node.actionTarget.clear();
    };
    auto buttonStyle = [](Node& node) {
        node.type = "button";
        node.bgColor = ImVec4(0.12f, 0.17f, 0.23f, 0.90f);
        node.hoverColor = ImVec4(0.18f, 0.32f, 0.40f, 0.95f);
        node.borderColor = ImVec4(0.35f, 0.48f, 0.62f, 0.55f);
        node.hoverBorderColor = ImVec4(0.45f, 0.95f, 0.82f, 0.90f);
        node.borderTopHeight = 2.0f;
        node.radius = 7.0f;
    };

    if (lower.find("components/dialogue_box") != std::string::npos) {
        m_canvasW = 1280;
        m_canvasH = 720;
        Node& box = makeNode("dialogue_box", "dialogue_box", "Dialogue Box", "content", 0, 88, 486, 1104, 172);
        panelStyle(box, ImVec4(0.04f, 0.05f, 0.08f, 0.78f));
        box.image = "Image/dialoguebox.png";
        Node& name = makeNode("speaker_name", "text", "Speaker Name", "controls", 0, 126, 456, 240, 44);
        textStyle(name, 24);
        name.text = "伊莉雅";
        Node& line = makeNode("dialogue_text", "text", "Dialogue Text", "controls", 1, 146, 536, 960, 74);
        textStyle(line, 28);
        line.text = "這裡是新版 UI Component Editor 產生的對話框。";
    } else if (lower.find("components/choice_list") != std::string::npos) {
        Node& title = makeNode("choice_title", "text", "Choice Title", "content", 0, 420, 178, 420, 46);
        textStyle(title, 28);
        title.text = "選擇接下來的行動";
        for (int i = 0; i < 4; ++i) {
            Node& choice = makeNode("choice_" + std::to_string(i + 1), "button", "Choice " + std::to_string(i + 1), "controls", i, 390, 250.0f + i * 72.0f, 500, 54);
            buttonStyle(choice);
            choice.text = i == 0 ? "繼續故事" : i == 1 ? "打開存檔" : i == 2 ? "查看回想" : "返回";
            choice.actionType = i == 0 ? "scene.start" : "ui.toggle";
            choice.actionTarget = i == 1 ? "save_menu" : i == 2 ? "backlog" : "";
        }
    } else if (lower.find("components/save_slot") != std::string::npos) {
        Node& slot = makeNode("save_slot_panel", "frame", "Save Slot", "content", 0, 180, 120, 920, 112);
        panelStyle(slot);
        Node& title = makeNode("slot_title", "text", "Slot Title", "controls", 0, 220, 142, 320, 34);
        textStyle(title, 24);
        title.text = "SAVE 01";
        Node& body = makeNode("slot_info", "text", "Slot Info", "controls", 1, 220, 184, 640, 28);
        textStyle(body, 18);
        body.text = "Chapter 1 / 00:12:42";
    } else if (lower.find("components/image_button") != std::string::npos) {
        Node& button = makeNode("image_button", "button", "Image Button", "controls", 0, 96, 96, 220, 62);
        buttonStyle(button);
        button.text = "Button";
    } else if (lower.find("save_load") != std::string::npos) {
        Node& panel = makeNode("save_load_panel", "frame", "Save Load Panel", "content", 0, 76, 58, 1128, 604);
        panelStyle(panel);
        Node& title = makeNode("save_load_title", "text", "Title", "controls", 0, 112, 84, 360, 54);
        textStyle(title, 36);
        title.text = "SAVE / LOAD";
        for (int i = 0; i < 6; ++i) {
            Node& slot = makeNode("slot_" + std::to_string(i + 1), "button", "Slot " + std::to_string(i + 1), "controls", i + 1, 128, 160.0f + i * 76.0f, 930, 58);
            buttonStyle(slot);
            slot.text = "Empty Slot " + std::to_string(i + 1);
            slot.actionType = "save.slot";
            slot.actionTarget = std::to_string(i + 1);
        }
        Node& back = makeNode("back_button", "button", "Back", "controls", 10, 1064, 596, 112, 44);
        buttonStyle(back);
        back.text = "Back";
        back.actionType = "ui.toggle";
        back.actionTarget = "close";
    } else if (lower.find("cg_gallery") != std::string::npos || lower.find("gallery") != std::string::npos) {
        Node& title = makeNode("gallery_title", "text", "Gallery Title", "content", 0, 82, 52, 420, 54);
        textStyle(title, 38);
        title.text = "CG GALLERY";
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 4; ++col) {
                const int index = row * 4 + col + 1;
                Node& tile = makeNode("cg_tile_" + std::to_string(index), "button", "CG Tile " + std::to_string(index), "controls", index, 92.0f + col * 286.0f, 146.0f + row * 216.0f, 238, 154);
                panelStyle(tile, ImVec4(0.08f, 0.10f, 0.13f, 0.88f));
                tile.text = "CG " + std::to_string(index);
                tile.actionType = "ui.set_state";
                tile.actionTarget = "cg_" + std::to_string(index);
            }
        }
    } else if (lower.find("play_hud") != std::string::npos) {
        m_backgroundColor = ImVec4(0, 0, 0, 0);
        Node& quick = makeNode("quick_toolbar", "frame", "Quick Toolbar", "controls", 0, 924, 18, 328, 52);
        panelStyle(quick, ImVec4(0.03f, 0.05f, 0.08f, 0.62f));
        Node& dialogue = makeNode("dialogue_component", "component", "Dialogue Component", "content", 0, 0, 0, 1280, 720);
        dialogue.component = "Scripts/ui/components/dialogue_box.lua";
        dialogue.actionType = "ui.toggle";
        dialogue.actionTarget.clear();
        const std::array<const char*, 4> buttons{ "Save", "Load", "Backlog", "Menu" };
        for (int i = 0; i < 4; ++i) {
            Node& button = makeNode(std::string("hud_") + buttons[i], "button", buttons[i], "controls", i + 1, 944.0f + i * 76.0f, 26, 68, 36);
            buttonStyle(button);
            button.text = buttons[i];
            button.fontSize = 17;
            button.actionType = i == 2 ? "ui.toggle" : i == 3 ? "scene.switch" : "save.open";
            button.actionTarget = i == 2 ? "backlog" : i == 3 ? "title" : std::string(buttons[i]);
        }
    } else if (lower.find("backlog") != std::string::npos) {
        Node& overlay = makeNode("backlog_overlay", "backlog", "Backlog Overlay", "background", 0, 0, 0, 1280, 720);
        panelStyle(overlay, ImVec4(0, 0, 0, 0.78f));
        Node& title = makeNode("backlog_title", "text", "Backlog Title", "content", 0, 64, 42, 360, 48);
        textStyle(title, 36);
        title.text = "HISTORY";
        for (int i = 0; i < 6; ++i) {
            Node& entry = makeNode("history_line_" + std::to_string(i + 1), "text", "History Line " + std::to_string(i + 1), "content", i + 1, 92, 126.0f + i * 72.0f, 1040, 44);
            textStyle(entry, 22);
            entry.text = i == 0 ? "伊莉雅: 這裡會顯示遊戲中的對話紀錄。" : "Narration line " + std::to_string(i + 1);
        }
    } else if (lower.find("notification") != std::string::npos) {
        m_backgroundColor = ImVec4(0, 0, 0, 0);
        Node& toast = makeNode("notification_toast", "frame", "Notification Toast", "content", 0, 856, 48, 360, 66);
        panelStyle(toast, ImVec4(0.05f, 0.07f, 0.10f, 0.92f));
        Node& stripe = makeNode("notification_stripe", "frame", "Accent", "controls", 0, 856, 48, 5, 66);
        panelStyle(stripe, ImVec4(0.34f, 0.86f, 1.0f, 1.0f));
        Node& text = makeNode("notification_text", "text", "Message", "controls", 1, 884, 68, 300, 28);
        textStyle(text, 20);
        text.text = "Notification message";
    } else {
        SeedBlankDocument();
    }
    RecalculateLayerOrders();
    m_revision++;
}

void UIDesigner::ResetToDefaults() {
    SeedDefaultScene();
    MarkDirty();
}

void UIDesigner::LoadDocument(const Json& json) {
    m_layers.clear();
    m_nodes.clear();
    m_selectedIds.clear();

    if (json.contains("canvas")) {
        const Json& canvas = json["canvas"];
        m_canvasW = canvas.value("w", canvas.value("width", m_canvasW));
        m_canvasH = canvas.value("h", canvas.value("height", m_canvasH));
        m_snapGrid = canvas.value("snapGrid", m_snapGrid);
        m_backgroundColor = ColorFromJson(canvas.value("backgroundColor", Json::array()), m_backgroundColor);
        m_backgroundImage = canvas.value("backgroundImage", canvas.value("image", m_backgroundImage));
    }

    for (const Json& item : json.value("layers", Json::array())) {
        Layer layer;
        layer.id = item.value("id", "layer");
        layer.name = item.value("name", layer.id);
        layer.visible = item.value("visible", true);
        layer.locked = item.value("locked", false);
        layer.expanded = item.value("expanded", true);
        m_layers.push_back(std::move(layer));
    }
    if (m_layers.empty()) {
        m_layers.push_back({ "background", "Background", true, false, true });
        m_layers.push_back({ "content", "Content", true, false, true });
        m_layers.push_back({ "hud", "HUD", true, false, true });
    }

    int maxIndex = 1;
    for (const Json& item : json.value("nodes", Json::array())) {
        Node node;
        node.id = item.value("id", "node_" + std::to_string(maxIndex));
        node.type = item.value("type", "button");
        node.name = item.value("name", node.id);
        node.layerId = item.value("layerId", "content");
        node.layer = item.value("layer", 0);
        node.order = item.value("order", 0);
        node.visible = item.value("visible", true);
        node.locked = item.value("locked", false);
        node.x = item.value("x", 0.0f);
        node.y = item.value("y", 0.0f);
        node.w = item.value("w", 220.0f);
        node.h = item.value("h", 56.0f);
        node.opacity = item.value("opacity", 255.0f);
        node.radius = item.value("radius", 8.0f);
        node.text = item.value("text", "");
        node.image = item.value("image", "");
        node.hoverImage = item.value("hoverImage", "");
        node.component = item.value("component", item.value("componentPath", ""));
        node.font = item.value("font", node.font);
        node.fontSize = item.value("fontSize", node.fontSize);
        node.bgColor = ColorFromJson(item.value("bgColor", Json::array()), node.bgColor);
        node.hoverColor = ColorFromJson(item.value("hoverColor", Json::array()), node.hoverColor);
        node.borderColor = ColorFromJson(item.value("borderColor", Json::array()), node.borderColor);
        node.hoverBorderColor = ColorFromJson(item.value("hoverBorderColor", Json::array()), node.hoverBorderColor);
        node.textColor = ColorFromJson(item.value("textColor", Json::array()), node.textColor);
        node.borderTopHeight = item.value("borderTopHeight", node.borderTopHeight);
        node.textShadow = item.value("textShadow", node.textShadow);
        node.state = item.value("state", "normal");
        if (item.contains("action")) {
            node.actionType = item["action"].value("type", node.actionType);
            node.actionTarget = item["action"].value("target", "");
            node.actionArgument = item["action"].value("argument", "");
        }
        for (const Json& clipJson : item.value("animations", Json::array())) {
            AnimationClip clip;
            clip.name = clipJson.value("name", clip.name);
            clip.duration = clipJson.value("duration", clip.duration);
            clip.delay = clipJson.value("delay", clip.delay);
            clip.easing = clipJson.value("easing", clip.easing);
            clip.loop = clipJson.value("loop", clip.loop);
            clip.trigger = clipJson.value("trigger", clip.trigger);
            for (const Json& keyJson : clipJson.value("keys", Json::array())) {
                AnimationKey key;
                key.time = keyJson.value("time", key.time);
                key.property = keyJson.value("property", key.property);
                key.value = keyJson.value("value", key.value);
                clip.keys.push_back(std::move(key));
            }
            node.animations.push_back(std::move(clip));
        }
        maxIndex = std::max(maxIndex, std::atoi(node.id.c_str()) + 1);
        m_nodes.push_back(std::move(node));
    }
    if (m_nodes.empty()) {
        SeedDefaultScene();
    }
    RecalculateLayerOrders();
    m_revision++;
}

Json UIDesigner::SaveDocument() const {
    Json document;
    document["version"] = 1;
    document["canvas"] = {
        { "w", m_canvasW },
        { "h", m_canvasH },
        { "snapGrid", m_snapGrid },
        { "backgroundColor", ColorToJson(m_backgroundColor) },
        { "backgroundImage", m_backgroundImage },
    };
    document["resources"] = Json::array();
    document["layers"] = Json::array();
    document["nodes"] = Json::array();
    document["components"] = Json::array({ "image", "text", "button", "panel", "group", "stack", "component", "dialogue_box", "choice_list", "backlog", "custom" });
    document["styles"] = Json::object();
    document["actions"] = Json::array({ "scene.start", "scene.switch", "game.exit", "save.open", "save.slot", "ui.toggle", "ui.set_state", "audio.play", "script.call" });
    document["animations"] = Json::object();

    for (const Layer& layer : m_layers) {
        document["layers"].push_back({
            { "id", layer.id },
            { "name", layer.name },
            { "visible", layer.visible },
            { "locked", layer.locked },
            { "expanded", layer.expanded },
        });
    }

    for (const Node& node : m_nodes) {
        Json item = {
            { "id", node.id },
            { "type", node.type },
            { "name", node.name },
            { "layerId", node.layerId },
            { "layer", node.layer },
            { "order", node.order },
            { "visible", node.visible },
            { "locked", node.locked },
            { "x", node.x },
            { "y", node.y },
            { "w", node.w },
            { "h", node.h },
            { "opacity", node.opacity },
            { "radius", node.radius },
            { "text", node.text },
            { "image", node.image },
            { "hoverImage", node.hoverImage },
            { "component", node.component },
            { "font", node.font },
            { "fontSize", node.fontSize },
            { "bgColor", ColorToJson(node.bgColor) },
            { "hoverColor", ColorToJson(node.hoverColor) },
            { "borderColor", ColorToJson(node.borderColor) },
            { "hoverBorderColor", ColorToJson(node.hoverBorderColor) },
            { "textColor", ColorToJson(node.textColor) },
            { "borderTopHeight", node.borderTopHeight },
            { "textShadow", node.textShadow },
            { "state", node.state },
            { "action", Json{ { "type", node.actionType }, { "target", node.actionTarget }, { "argument", node.actionArgument } } },
            { "animations", Json::array() },
        };
        for (const AnimationClip& clip : node.animations) {
            Json clipJson = {
                { "name", clip.name },
                { "duration", clip.duration },
                { "delay", clip.delay },
                { "easing", clip.easing },
                { "loop", clip.loop },
                { "trigger", clip.trigger },
                { "keys", Json::array() },
            };
            for (const AnimationKey& key : clip.keys) {
                clipJson["keys"].push_back({ { "time", key.time }, { "property", key.property }, { "value", key.value } });
            }
            item["animations"].push_back(std::move(clipJson));
        }
        document["nodes"].push_back(std::move(item));
    }
    return document;
}

void UIDesigner::Save() {
    const fs::path path = DocumentPath();
    if (path.empty()) {
        return;
    }
    try {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << std::setw(2) << SaveDocument() << "\n";
        m_dirty = false;
        Log("Saved UI document: " + path.string());
    } catch (const std::exception& exception) {
        Log("UI save failed: " + std::string(exception.what()));
    }
}

void UIDesigner::Reload() {
    LoadOrCreate();
}

bool UIDesigner::OpenDocument(const std::string& runtimePath) {
    if (!m_project || !m_project->IsOpen()) {
        return false;
    }
    const std::string normalized = NormalizeUiRuntimePath(runtimePath);
    if (normalized.empty()) {
        return false;
    }
    if (m_dirty) {
        Save();
    }
    m_documentRuntimePath = normalized;
    m_documentPathInput = normalized;
    LoadOrCreate();
    return true;
}

bool UIDesigner::NewDocument(const std::string& runtimePath) {
    if (!m_project || !m_project->IsOpen()) {
        return false;
    }
    const std::string normalized = NormalizeUiRuntimePath(runtimePath);
    if (normalized.empty()) {
        return false;
    }
    if (m_dirty) {
        Save();
    }
    m_documentRuntimePath = normalized;
    m_documentPathInput = normalized;
    SeedTemplateDocument(normalized);
    MarkDirty();
    Save();
    return true;
}

void UIDesigner::Render() {
    if (!m_project || !m_project->IsOpen()) {
        ImGui::TextDisabled("Open a project to edit a UI scene.");
        return;
    }
    RenderCanvasToolbar();
    RenderCanvas();
}

void UIDesigner::RenderCanvasToolbar() {
    ImGui::BeginChild("ui-canvas-toolbar", ImVec2(0, 76), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetNextItemWidth(330.0f);
    ImGui::InputTextWithHint("##ui-document", "UI/title_menu.pxui", &m_documentPathInput);
    ImGui::SameLine();
    if (ImGui::Button("Open UI")) OpenDocument(m_documentPathInput);
    ImGui::SameLine();
    if (ImGui::Button("New Blank")) NewDocument(m_documentPathInput);
    ImGui::SameLine();
    ImGui::TextDisabled("%s -> %s%s", CurrentDocumentRuntimePath().c_str(), GeneratedSceneScriptPath().c_str(), m_dirty ? " *" : "");

    if (ImGui::Button("Frame")) FrameSelection();
    ImGui::SameLine();
    if (ImGui::Button("Panel")) AddNodeAt("panel", ImVec2(120, 120));
    ImGui::SameLine();
    if (ImGui::Button("Text")) AddNodeAt("text", ImVec2(160, 160));
    ImGui::SameLine();
    if (ImGui::Button("Button")) AddNodeAt("button", ImVec2(220, 220));
    ImGui::SameLine();
    if (ImGui::Button("Image")) {
        const std::string selected = m_selectedResource ? m_selectedResource() : "";
        AddNodeAt("image", ImVec2(280, 180), selected);
    }
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &m_gridVisible);
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &m_snapEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Guides", &m_showGuides);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Zoom", &m_zoom, 0.20f, 2.50f, "%.0f%%", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::DragFloat("Grid px", &m_snapGrid, 1.0f, 1.0f, 64.0f, "%.0f")) MarkDirty();
    ImGui::SameLine();
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    ImGui::EndChild();
}

void UIDesigner::RenderCanvas() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("ui-canvas", available, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 areaMin = ImGui::GetCursorScreenPos();
    const ImVec2 areaSize = ImGui::GetContentRegionAvail();
    const ImRect area(areaMin, areaMin + areaSize);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(area.Min, area.Max, IM_COL32(9, 11, 15, 255));
    drawList->AddRect(area.Min, area.Max, IM_COL32(34, 40, 49, 255));

    const ImVec2 centered((area.GetWidth() - static_cast<float>(m_canvasW) * m_zoom) * 0.5f, (area.GetHeight() - static_cast<float>(m_canvasH) * m_zoom) * 0.5f);
    const ImVec2 origin = area.Min + centered + m_pan;
    const ImRect canvasRect(origin, origin + ImVec2(static_cast<float>(m_canvasW) * m_zoom, static_cast<float>(m_canvasH) * m_zoom));

    drawList->PushClipRect(area.Min, area.Max, true);
    bool runtimeDrawn = false;
    if (m_runtimePreview) {
        int previewW = 0;
        int previewH = 0;
        if (ImTextureID previewTexture = m_runtimePreview(canvasRect.Min, canvasRect.GetSize(), &previewW, &previewH)) {
            drawList->AddImage(previewTexture, canvasRect.Min, canvasRect.Max);
            runtimeDrawn = true;
        }
    }
    if (!runtimeDrawn) {
        drawList->AddRectFilled(canvasRect.Min, canvasRect.Max, ImGui::ColorConvertFloat4ToU32(m_backgroundColor));
        if (!m_backgroundImage.empty() && m_textureResolver) {
            int imageW = 0;
            int imageH = 0;
            if (ImTextureID backgroundTexture = m_textureResolver(m_backgroundImage, &imageW, &imageH)) {
                drawList->AddImage(backgroundTexture, canvasRect.Min, canvasRect.Max);
            }
        }
    }
    if (m_gridVisible) {
        RenderGrid(origin, m_zoom, drawList, area);
    }

    if (!runtimeDrawn) {
        std::vector<Node*> sorted;
        SortNodesForRuntime(sorted);
        for (Node* node : sorted) {
            if (node && node->visible) {
                const Layer* layer = FindLayer(node->layerId);
                if (!layer || layer->visible) {
                    RenderNodeOnCanvas(*node, origin, m_zoom, drawList, IsSelected(node->id));
                }
            }
        }
    }
    DrawSelectionAndGuides(origin, m_zoom, drawList);
    drawList->AddRect(canvasRect.Min, canvasRect.Max, IM_COL32(86, 100, 116, 255), 0.0f, 0, 1.5f);
    drawList->PopClipRect();

    ImGui::SetCursorScreenPos(area.Min);
    ImGui::InvisibleButton("canvas-hitbox", areaSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    HandleCanvasInput(origin, m_zoom, canvasRect);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
            const char* path = static_cast<const char*>(payload->Data);
            AddNodeAt(RuntimeTypeFromAsset(path), ScreenToCanvas(ImGui::GetMousePos(), origin, m_zoom), path);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();
}

void UIDesigner::RenderGrid(const ImVec2& origin, float scale, ImDrawList* drawList, const ImRect& clipRect) const {
    const float step = std::max(2.0f, m_snapGrid * scale);
    const float major = step * 8.0f;
    const ImU32 minorColor = IM_COL32(255, 255, 255, 15);
    const ImU32 majorColor = IM_COL32(120, 178, 190, 35);
    const ImRect canvasRect(origin, origin + ImVec2(static_cast<float>(m_canvasW) * scale, static_cast<float>(m_canvasH) * scale));
    const float xStart = std::max(canvasRect.Min.x, clipRect.Min.x);
    const float yStart = std::max(canvasRect.Min.y, clipRect.Min.y);
    const float xEnd = std::min(canvasRect.Max.x, clipRect.Max.x);
    const float yEnd = std::min(canvasRect.Max.y, clipRect.Max.y);
    for (float x = origin.x + std::fmod(std::max(0.0f, xStart - origin.x), step); x < xEnd; x += step) {
        const float local = x - origin.x;
        const bool isMajor = std::fmod(std::abs(local), major) < 0.5f;
        drawList->AddLine(ImVec2(x, yStart), ImVec2(x, yEnd), isMajor ? majorColor : minorColor);
    }
    for (float y = origin.y + std::fmod(std::max(0.0f, yStart - origin.y), step); y < yEnd; y += step) {
        const float local = y - origin.y;
        const bool isMajor = std::fmod(std::abs(local), major) < 0.5f;
        drawList->AddLine(ImVec2(xStart, y), ImVec2(xEnd, y), isMajor ? majorColor : minorColor);
    }
}

void UIDesigner::RenderNodeOnCanvas(Node& node, const ImVec2& origin, float scale, ImDrawList* drawList, bool selected) {
    const ImVec2 min = CanvasToScreen(ImVec2(node.x, node.y), origin, scale);
    const ImVec2 max = CanvasToScreen(ImVec2(node.x + node.w, node.y + node.h), origin, scale);
    const ImRect rect(min, max);
    const Layer* layer = FindLayer(node.layerId);
    const bool locked = node.locked || (layer && layer->locked);
    const bool hovered = ImGui::IsMouseHoveringRect(rect.Min, rect.Max);
    ImU32 fill = ImGui::ColorConvertFloat4ToU32(hovered ? node.hoverColor : node.bgColor);
    ImU32 stroke = selected ? IM_COL32(86, 232, 212, 255) : ImGui::ColorConvertFloat4ToU32(hovered ? node.hoverBorderColor : node.borderColor);

    const std::string image = (hovered && !node.hoverImage.empty()) ? node.hoverImage : node.image;
    if (!image.empty() && m_textureResolver) {
        int texW = 0;
        int texH = 0;
        const ImTextureID texture = m_textureResolver(image, &texW, &texH);
        if (texture) {
            drawList->AddImage(texture, rect.Min, rect.Max);
        } else {
            drawList->AddRectFilled(rect.Min, rect.Max, IM_COL32(30, 38, 46, 230), std::min(node.radius * scale, 8.0f));
        }
    } else if (node.type != "text") {
        drawList->AddRectFilled(rect.Min, rect.Max, fill, std::min(node.radius * scale, 8.0f));
    }
    if (node.borderTopHeight > 0.0f && node.type != "text") {
        drawList->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + node.borderTopHeight * scale), stroke);
    }

    const bool shouldDrawText = node.type == "text" || !node.text.empty();
    if (shouldDrawText && (node.type == "text" || node.type == "button" || node.type == "dialogue_box" || node.type == "choice_list" || node.type == "backlog")) {
        const std::string label = node.text.empty() ? node.name : node.text;
        const ImVec2 textPos = node.type == "button"
            ? ImVec2(rect.Min.x + std::max(10.0f, (rect.GetWidth() - ImGui::CalcTextSize(label.c_str()).x) * 0.5f), rect.Min.y + std::max(6.0f, (rect.GetHeight() - ImGui::GetTextLineHeight()) * 0.5f))
            : (node.type == "text" ? rect.Min : rect.Min + ImVec2(12.0f * scale, 10.0f * scale));
        drawList->AddText(textPos, ImGui::ColorConvertFloat4ToU32(node.textColor), label.c_str());
    }

    drawList->AddRect(rect.Min, rect.Max, stroke, std::min(node.radius * scale, 8.0f), 0, selected ? 2.0f : 1.0f);
    if (locked) {
        drawList->AddRectFilled(rect.Min, rect.Min + ImVec2(22, 22), IM_COL32(0, 0, 0, 120), 4.0f);
        drawList->AddText(rect.Min + ImVec2(5, 2), IM_COL32(230, 235, 240, 255), "L");
    }
}

void UIDesigner::HandleCanvasInput(const ImVec2& origin, float scale, const ImRect& canvasRect) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsItemHovered();
    const bool appendSelection = io.KeyShift || io.KeyCtrl;

    if (hovered && io.MouseWheel != 0.0f) {
        const float oldZoom = m_zoom;
        m_zoom = std::clamp(m_zoom + io.MouseWheel * 0.08f, 0.20f, 2.50f);
        const ImVec2 before = ScreenToCanvas(mouse, origin, oldZoom);
        const ImVec2 after = ScreenToCanvas(mouse, origin, m_zoom);
        m_pan += (after - before) * m_zoom;
    }

    auto hitNode = [&]() -> Node* {
        std::vector<Node*> sorted;
        SortNodesForRuntime(sorted);
        for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
            Node* node = *it;
            if (!node || !node->visible) continue;
            const Layer* layer = FindLayer(node->layerId);
            if (layer && (!layer->visible || layer->locked)) continue;
            if (node->locked) continue;
            const ImRect rect(CanvasToScreen(ImVec2(node->x, node->y), origin, scale), CanvasToScreen(ImVec2(node->x + node->w, node->y + node->h), origin, scale));
            if (rect.Contains(mouse)) return node;
        }
        return nullptr;
    };

    auto hitResize = [&]() -> Node* {
        Node* node = PrimarySelection();
        if (!node || node->locked) return nullptr;
        const Layer* layer = FindLayer(node->layerId);
        if (layer && layer->locked) return nullptr;
        const ImVec2 corner = CanvasToScreen(ImVec2(node->x + node->w, node->y + node->h), origin, scale);
        const ImRect handle(corner - ImVec2(9, 9), corner + ImVec2(9, 9));
        return handle.Contains(mouse) ? node : nullptr;
    };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_draggingCanvas = true;
        m_dragStartMouse = mouse;
        m_dragStartPan = m_pan;
    }
    if (m_draggingCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        m_pan = m_dragStartPan + (mouse - m_dragStartMouse);
    }
    if (m_draggingCanvas && ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
        m_draggingCanvas = false;
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (Node* resize = hitResize()) {
            m_resizingNode = true;
            m_dragStartMouse = mouse;
            m_dragStartCanvas = ScreenToCanvas(mouse, origin, scale);
            m_dragStartNodes = { *resize };
        } else if (Node* node = hitNode()) {
            SelectNode(node->id, appendSelection);
            m_draggingNode = true;
            m_dragStartMouse = mouse;
            m_dragStartCanvas = ScreenToCanvas(mouse, origin, scale);
            m_dragStartNodes.clear();
            for (const std::string& id : m_selectedIds) {
                if (Node* selected = FindNode(id)) {
                    m_dragStartNodes.push_back(*selected);
                }
            }
        } else {
            if (!appendSelection) ClearSelection();
            m_boxSelecting = true;
            m_selectionBoxStart = ScreenToCanvas(mouse, origin, scale);
        }
    }

    if (m_draggingNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 now = ScreenToCanvas(mouse, origin, scale);
        ImVec2 delta = now - m_dragStartCanvas;
        m_activeGuides.clear();
        if (Node* primary = PrimarySelection()) {
            Node moving = *primary;
            if (const auto start = std::find_if(m_dragStartNodes.begin(), m_dragStartNodes.end(), [&](const Node& item) { return item.id == primary->id; }); start != m_dragStartNodes.end()) {
                moving.x = start->x + delta.x;
                moving.y = start->y + delta.y;
            }
            float snapDx = 0.0f;
            float snapDy = 0.0f;
            if (m_showGuides) {
                m_activeGuides = BuildGuides(moving, snapDx, snapDy);
                delta.x += snapDx;
                delta.y += snapDy;
            }
        }
        for (const Node& start : m_dragStartNodes) {
            if (Node* node = FindNode(start.id)) {
                node->x = start.x + delta.x;
                node->y = start.y + delta.y;
                if (m_snapEnabled) {
                    node->x = Snap(node->x, m_snapGrid);
                    node->y = Snap(node->y, m_snapGrid);
                }
            }
        }
        MarkDirty();
    }
    if (m_draggingNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_draggingNode = false;
        m_activeGuides.clear();
    }

    if (m_resizingNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (!m_dragStartNodes.empty()) {
            const ImVec2 now = ScreenToCanvas(mouse, origin, scale);
            if (Node* node = FindNode(m_dragStartNodes[0].id)) {
                node->w = std::max(kMinNodeSize, m_dragStartNodes[0].w + (now.x - m_dragStartCanvas.x));
                node->h = std::max(kMinNodeSize, m_dragStartNodes[0].h + (now.y - m_dragStartCanvas.y));
                if (m_snapEnabled) {
                    node->w = Snap(node->w, m_snapGrid);
                    node->h = Snap(node->h, m_snapGrid);
                }
                MarkDirty();
            }
        }
    }
    if (m_resizingNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_resizingNode = false;
    }

    if (m_boxSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 end = ScreenToCanvas(mouse, origin, scale);
        const ImRect selectRect(ImMin(m_selectionBoxStart, end), ImMax(m_selectionBoxStart, end));
        m_selectedIds.clear();
        for (const Node& node : m_nodes) {
            if (selectRect.Overlaps(NodeRect(node))) {
                m_selectedIds.push_back(node.id);
            }
        }
    }
    if (m_boxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_boxSelecting = false;
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && canvasRect.Contains(mouse)) {
        AddNodeAt("button", ScreenToCanvas(mouse, origin, scale));
    }
}

void UIDesigner::DrawSelectionAndGuides(const ImVec2& origin, float scale, ImDrawList* drawList) {
    for (const std::string& id : m_selectedIds) {
        if (Node* node = FindNode(id)) {
            const ImRect rect(CanvasToScreen(ImVec2(node->x, node->y), origin, scale), CanvasToScreen(ImVec2(node->x + node->w, node->y + node->h), origin, scale));
            drawList->AddRect(rect.Min, rect.Max, IM_COL32(88, 230, 207, 255), std::min(node->radius * scale, 8.0f), 0, 2.0f);
            DrawResizeHandles(origin, scale, drawList, *node);
        }
    }
    for (const GuideLine& guide : m_activeGuides) {
        if (guide.vertical) {
            const float x = origin.x + guide.pos * scale;
            drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + static_cast<float>(m_canvasH) * scale), IM_COL32(255, 214, 116, 220), 1.0f);
        } else {
            const float y = origin.y + guide.pos * scale;
            drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + static_cast<float>(m_canvasW) * scale, y), IM_COL32(255, 214, 116, 220), 1.0f);
        }
    }
    if (m_boxSelecting) {
        const ImVec2 mouseCanvas = ScreenToCanvas(ImGui::GetMousePos(), origin, scale);
        const ImRect rect(ImMin(m_selectionBoxStart, mouseCanvas), ImMax(m_selectionBoxStart, mouseCanvas));
        const ImVec2 min = CanvasToScreen(rect.Min, origin, scale);
        const ImVec2 max = CanvasToScreen(rect.Max, origin, scale);
        drawList->AddRectFilled(min, max, IM_COL32(88, 230, 207, 28));
        drawList->AddRect(min, max, IM_COL32(88, 230, 207, 180));
    }
}

void UIDesigner::DrawResizeHandles(const ImVec2& origin, float scale, ImDrawList* drawList, Node& node) {
    if (&node != PrimarySelection()) {
        return;
    }
    const ImVec2 corner = CanvasToScreen(ImVec2(node.x + node.w, node.y + node.h), origin, scale);
    drawList->AddRectFilled(corner - ImVec2(6, 6), corner + ImVec2(6, 6), IM_COL32(88, 230, 207, 255), 2.0f);
}

void UIDesigner::RenderDocumentPanel() {
    ImGui::SeparatorText("UI Documents");
    ImGui::TextWrapped("%s", CurrentDocumentRuntimePath().c_str());
    ImGui::TextDisabled("%s%s", GeneratedSceneScriptPath().c_str(), m_dirty ? " *" : "");

    struct Preset {
        const char* label;
        const char* path;
    };
    static constexpr std::array<Preset, 10> presets{{
        { "Title", "UI/title_menu.pxui" },
        { "Save/Load", "UI/save_load_menu.pxui" },
        { "CG Gallery", "UI/cg_gallery.pxui" },
        { "Play HUD", "UI/play_hud.pxui" },
        { "Backlog", "UI/backlog.pxui" },
        { "Notification", "UI/notification.pxui" },
        { "Dialogue Box", "UI/components/dialogue_box.pxui" },
        { "Choice List", "UI/components/choice_list.pxui" },
        { "Save Slot", "UI/components/save_slot.pxui" },
        { "Image Button", "UI/components/image_button.pxui" },
    }};

    const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    for (size_t i = 0; i < presets.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool exists = m_project && fs::exists(m_project->DataRoot() / fs::path(presets[i].path));
        const std::string label = std::string(exists ? "" : "+ ") + presets[i].label;
        if (ImGui::Button(label.c_str(), ImVec2(buttonWidth, 0))) {
            if (exists) {
                OpenDocument(presets[i].path);
            } else {
                NewDocument(presets[i].path);
            }
        }
        if (i % 2 == 0) {
            ImGui::SameLine();
        }
        ImGui::PopID();
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ui-doc-search", "Search UI documents", &m_documentSearch);
    const std::vector<std::string> documents = EnumerateUiDocuments();
    if (documents.empty()) {
        ImGui::TextDisabled("No .pxui files under Data/UI yet.");
        return;
    }
    ImGui::BeginChild("ui-document-list", ImVec2(0, 132), ImGuiChildFlags_Borders);
    for (const std::string& runtimePath : documents) {
        if (!StringContains(runtimePath, m_documentSearch)) {
            continue;
        }
        const bool selected = runtimePath == CurrentDocumentRuntimePath();
        std::string label = runtimePath;
        if (selected && m_dirty) {
            label += " *";
        }
        if (ImGui::Selectable(label.c_str(), selected)) {
            OpenDocument(runtimePath);
        }
    }
    ImGui::EndChild();
}

std::vector<UIDesigner::GuideLine> UIDesigner::BuildGuides(const Node& moving, float& snapDx, float& snapDy) const {
    snapDx = 0.0f;
    snapDy = 0.0f;
    std::vector<GuideLine> guides;
    if (!m_showGuides) {
        return guides;
    }
    const std::array<float, 3> movingX{ moving.x, moving.x + moving.w * 0.5f, moving.x + moving.w };
    const std::array<float, 3> movingY{ moving.y, moving.y + moving.h * 0.5f, moving.y + moving.h };
    std::vector<float> targetsX{ 0.0f, static_cast<float>(m_canvasW) * 0.5f, static_cast<float>(m_canvasW) };
    std::vector<float> targetsY{ 0.0f, static_cast<float>(m_canvasH) * 0.5f, static_cast<float>(m_canvasH) };
    for (const Node& node : m_nodes) {
        if (node.id == moving.id || !node.visible) continue;
        targetsX.push_back(node.x);
        targetsX.push_back(node.x + node.w * 0.5f);
        targetsX.push_back(node.x + node.w);
        targetsY.push_back(node.y);
        targetsY.push_back(node.y + node.h * 0.5f);
        targetsY.push_back(node.y + node.h);
    }

    float bestDx = m_snapDistance + 1.0f;
    for (float target : targetsX) {
        for (float source : movingX) {
            const float delta = target - source;
            if (std::abs(delta) < std::abs(bestDx) && std::abs(delta) <= m_snapDistance) {
                bestDx = delta;
                guides = { { true, target } };
            }
        }
    }
    float bestDy = m_snapDistance + 1.0f;
    for (float target : targetsY) {
        for (float source : movingY) {
            const float delta = target - source;
            if (std::abs(delta) < std::abs(bestDy) && std::abs(delta) <= m_snapDistance) {
                bestDy = delta;
                guides.push_back({ false, target });
            }
        }
    }
    if (std::abs(bestDx) <= m_snapDistance) snapDx = bestDx;
    if (std::abs(bestDy) <= m_snapDistance) snapDy = bestDy;
    return guides;
}

void UIDesigner::RenderLayerPanel() {
    if (!m_project || !m_project->IsOpen()) {
        ImGui::TextDisabled("No project.");
        return;
    }
    RenderDocumentPanel();
    ImGui::SeparatorText("Layers");
    if (ImGui::Button("+ Layer")) {
        const std::string id = "layer_" + std::to_string(static_cast<int>(m_layers.size() + 1));
        m_layers.push_back({ id, "Layer " + std::to_string(m_layers.size() + 1), true, false, true });
        MarkDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Sel")) DeleteSelected();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##layer-search", "Search layers or nodes", &m_layerSearch);
    ImGui::Separator();

    for (int layerIndex = static_cast<int>(m_layers.size()) - 1; layerIndex >= 0; --layerIndex) {
        Layer& layer = m_layers[layerIndex];
        bool hasVisibleNode = StringContains(layer.name, m_layerSearch);
        for (const Node& node : m_nodes) {
            if (node.layerId == layer.id && StringContains(node.name + " " + node.id, m_layerSearch)) {
                hasVisibleNode = true;
                break;
            }
        }
        if (!hasVisibleNode) continue;

        ImGui::PushID(layer.id.c_str());
        if (ImGui::SmallButton(layer.visible ? "V" : "-")) {
            layer.visible = !layer.visible;
            MarkDirty();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(layer.locked ? "L" : "U")) {
            layer.locked = !layer.locked;
            MarkDirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-62.0f);
        if (ImGui::InputText("##layer-name", &layer.name)) MarkDirty();
        ImGui::SameLine();
        if (ImGui::SmallButton("^") && layerIndex + 1 < static_cast<int>(m_layers.size())) MoveLayer(layerIndex, layerIndex + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("v") && layerIndex > 0) MoveLayer(layerIndex, layerIndex - 1);

        if (layer.expanded) {
            std::vector<Node*> nodes;
            for (Node& node : m_nodes) {
                if (node.layerId == layer.id && StringContains(node.name + " " + node.id, m_layerSearch)) {
                    nodes.push_back(&node);
                }
            }
            std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) { return a->order > b->order; });
            ImGui::Indent(18.0f);
            for (Node* node : nodes) {
                ImGui::PushID(node->id.c_str());
                if (ImGui::SmallButton(node->visible ? "V" : "-")) {
                    node->visible = !node->visible;
                    MarkDirty();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(node->locked ? "L" : "U")) {
                    node->locked = !node->locked;
                    MarkDirty();
                }
                ImGui::SameLine();
                if (ImGui::Selectable(node->name.c_str(), IsSelected(node->id), ImGuiSelectableFlags_SpanAvailWidth)) {
                    SelectNode(node->id, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("PX_UI_NODE", node->id.c_str(), node->id.size() + 1);
                    ImGui::TextUnformatted(node->name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PX_UI_NODE")) {
                        const char* draggedId = static_cast<const char*>(payload->Data);
                        if (Node* dragged = FindNode(draggedId)) {
                            dragged->layerId = layer.id;
                            MoveNodeOrder(*dragged, node->order - dragged->order);
                            RecalculateLayerOrders();
                            SelectNode(dragged->id, false);
                            MarkDirty();
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
            ImGui::Unindent(18.0f);
        }
        ImGui::PopID();
    }
}

void UIDesigner::RenderInspector() {
    Node* node = PrimarySelection();
    if (!node) {
        ImGui::TextDisabled("Select a UI element on the canvas or in Layers.");
        ImGui::SeparatorText("Canvas");
        if (ImGui::DragInt("Width", &m_canvasW, 1.0f, 320, 7680)) MarkDirty();
        if (ImGui::DragInt("Height", &m_canvasH, 1.0f, 240, 4320)) MarkDirty();
        if (ImGui::ColorEdit4("Background", &m_backgroundColor.x)) MarkDirty();
        if (ImGui::InputText("Background Image", &m_backgroundImage)) MarkDirty();
        return;
    }

    RenderTextRow("Name", node->name);
    ImGui::TextDisabled("%s", node->id.c_str());
    RenderNodeTypeCombo(*node);
    if (ImGui::BeginCombo("Layer", node->layerId.c_str())) {
        for (Layer& layer : m_layers) {
            const bool selected = node->layerId == layer.id;
            if (ImGui::Selectable(layer.name.c_str(), selected)) {
                node->layerId = layer.id;
                RecalculateLayerOrders();
                MarkDirty();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::DragFloat("X", &node->x, 1.0f);
        changed |= ImGui::DragFloat("Y", &node->y, 1.0f);
        changed |= ImGui::DragFloat("W", &node->w, 1.0f, kMinNodeSize, 7680.0f);
        changed |= ImGui::DragFloat("H", &node->h, 1.0f, kMinNodeSize, 4320.0f);
        if (ImGui::Button("Reset Transform")) {
            node->x = 96.0f;
            node->y = 96.0f;
            node->w = 220.0f;
            node->h = 56.0f;
            changed = true;
        }
        if (changed) MarkDirty();
    }

    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::DragFloat("Opacity", &node->opacity, 1.0f, 0.0f, 255.0f);
        changed |= ImGui::DragFloat("Radius", &node->radius, 0.5f, 0.0f, 24.0f);
        changed |= ImGui::ColorEdit4("Fill", &node->bgColor.x);
        changed |= ImGui::ColorEdit4("Hover", &node->hoverColor.x);
        changed |= ImGui::ColorEdit4("Border", &node->borderColor.x);
        changed |= ImGui::ColorEdit4("Hover Border", &node->hoverBorderColor.x);
        changed |= ImGui::DragFloat("Top Border", &node->borderTopHeight, 0.25f, 0.0f, 12.0f);
        changed |= ImGui::Checkbox("Visible", &node->visible);
        changed |= ImGui::Checkbox("Locked", &node->locked);
        if (changed) MarkDirty();
    }

    if (ImGui::CollapsingHeader("Text", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::InputTextMultiline("Copy", &node->text, ImVec2(-1, 74));
        changed |= ImGui::InputText("Font", &node->font);
        changed |= ImGui::DragInt("Size", &node->fontSize, 1.0f, 8, 128);
        changed |= ImGui::ColorEdit4("Text Color", &node->textColor.x);
        changed |= ImGui::Checkbox("Shadow", &node->textShadow);
        if (changed) MarkDirty();
    }

    if (ImGui::CollapsingHeader("Image", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::InputText("Asset", &node->image);
        changed |= ImGui::InputText("Hover Asset", &node->hoverImage);
        if (ImGui::Button("Use Selected Resource")) {
            const std::string selected = m_selectedResource ? m_selectedResource() : "";
            if (!selected.empty()) {
                node->image = selected;
                node->type = "image";
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Selected As Hover")) {
            const std::string selected = m_selectedResource ? m_selectedResource() : "";
            if (!selected.empty()) {
                node->hoverImage = selected;
                changed = true;
            }
        }
        if (changed) MarkDirty();
    }

    if (ImGui::CollapsingHeader("Component", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = false;
        changed |= ImGui::InputText("Component Script", &node->component);
        if (ImGui::Button("Use Selected Component")) {
            const std::string selected = m_selectedResource ? m_selectedResource() : "";
            if (!selected.empty()) {
                std::string path = selected;
                std::replace(path.begin(), path.end(), '\\', '/');
                std::string lowerPath = path;
                std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                node->component = lowerPath.ends_with(".pxui") ? GeneratedScriptPathForRuntimePath(path) : path;
                node->type = "component";
                changed = true;
            }
        }
        if (changed) MarkDirty();
    }

    if (ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputText("State", &node->state)) MarkDirty();
    }

    if (ImGui::CollapsingHeader("Action", ImGuiTreeNodeFlags_DefaultOpen)) {
        RenderActionTypeCombo(*node);
        bool changed = false;
        changed |= ImGui::InputText("Target", &node->actionTarget);
        changed |= ImGui::InputText("Argument", &node->actionArgument);
        if (changed) MarkDirty();
    }
}

void UIDesigner::RenderActionsPanel() {
    static const std::array<const char*, 9> actionTypes{
        "scene.start", "scene.switch", "game.exit", "save.open", "save.slot", "ui.toggle", "ui.set_state", "audio.play", "script.call",
    };
    ImGui::TextDisabled("Action Types");
    for (const char* action : actionTypes) {
        ImGui::BulletText("%s", action);
    }
    if (Node* node = PrimarySelection()) {
        ImGui::SeparatorText("Selected Element");
        RenderActionTypeCombo(*node);
        if (ImGui::InputText("Target", &node->actionTarget)) MarkDirty();
        if (ImGui::InputText("Argument", &node->actionArgument)) MarkDirty();
    }
}

void UIDesigner::RenderAnimationPanel() {
    Node* node = PrimarySelection();
    if (!node) {
        ImGui::TextDisabled("Select an element to edit animation tracks.");
        return;
    }
    if (ImGui::Button("+ Clip")) {
        node->animations.push_back({ "Animation", 0.4f, 0.0f, "outCubic", false, "scene.enter", { { 0.0f, "opacity", 0.0f }, { 0.4f, "opacity", 255.0f } } });
        MarkDirty();
    }
    for (size_t clipIndex = 0; clipIndex < node->animations.size(); ++clipIndex) {
        AnimationClip& clip = node->animations[clipIndex];
        ImGui::PushID(static_cast<int>(clipIndex));
        if (ImGui::CollapsingHeader(clip.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            bool changed = false;
            changed |= ImGui::InputText("Name", &clip.name);
            changed |= ImGui::DragFloat("Duration", &clip.duration, 0.01f, 0.01f, 30.0f, "%.2fs");
            changed |= ImGui::DragFloat("Delay", &clip.delay, 0.01f, 0.0f, 30.0f, "%.2fs");
            changed |= ImGui::InputText("Easing", &clip.easing);
            changed |= ImGui::InputText("Trigger", &clip.trigger);
            changed |= ImGui::Checkbox("Loop", &clip.loop);
            if (ImGui::Button("+ Key")) {
                clip.keys.push_back({ clip.duration, "opacity", 255.0f });
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove Clip")) {
                node->animations.erase(node->animations.begin() + static_cast<std::ptrdiff_t>(clipIndex));
                MarkDirty();
                ImGui::PopID();
                break;
            }
            for (size_t keyIndex = 0; keyIndex < clip.keys.size(); ++keyIndex) {
                AnimationKey& key = clip.keys[keyIndex];
                ImGui::PushID(static_cast<int>(keyIndex));
                ImGui::SetNextItemWidth(72.0f);
                changed |= ImGui::DragFloat("T", &key.time, 0.01f, 0.0f, std::max(clip.duration, 0.01f), "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                changed |= ImGui::InputText("Prop", &key.property);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(92.0f);
                changed |= ImGui::DragFloat("Value", &key.value, 1.0f);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    clip.keys.erase(clip.keys.begin() + static_cast<std::ptrdiff_t>(keyIndex));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (changed) MarkDirty();
        }
        ImGui::PopID();
    }
}

void UIDesigner::AddNodeAt(const std::string& type, const ImVec2& canvasPoint, const std::string& runtimeAsset) {
    Node node;
    node.id = SanitizeId(type) + "_" + std::to_string(m_nextNodeIndex++);
    node.type = type == "panel" ? "frame" : type;
    node.name = type == "panel" ? "Panel" : type == "text" ? "Text" : type == "image" ? "Image" : "Button";
    node.layerId = m_layers.empty() ? "content" : m_layers.back().id;
    node.layer = static_cast<int>(m_layers.size()) - 1;
    node.order = static_cast<int>(m_nodes.size());
    node.x = m_snapEnabled ? Snap(canvasPoint.x, m_snapGrid) : canvasPoint.x;
    node.y = m_snapEnabled ? Snap(canvasPoint.y, m_snapGrid) : canvasPoint.y;
    if (node.type == "text") {
        node.w = 360.0f;
        node.h = 48.0f;
        node.text = "New Text";
        node.bgColor = ImVec4(0, 0, 0, 0);
    } else if (node.type == "image") {
        node.w = 320.0f;
        node.h = 180.0f;
        node.image = runtimeAsset;
        node.bgColor = ImVec4(0.08f, 0.10f, 0.13f, 1.0f);
    } else if (node.type == "frame") {
        node.w = 360.0f;
        node.h = 220.0f;
        node.bgColor = ImVec4(0.08f, 0.11f, 0.14f, 0.82f);
    } else {
        node.text = "Button";
    }
    m_nodes.push_back(std::move(node));
    SelectNode(m_nodes.back().id, false);
    RecalculateLayerOrders();
    MarkDirty();
}

void UIDesigner::DeleteSelected() {
    if (m_selectedIds.empty()) return;
    std::unordered_set<std::string> selected(m_selectedIds.begin(), m_selectedIds.end());
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) { return selected.contains(node.id); }), m_nodes.end());
    m_selectedIds.clear();
    RecalculateLayerOrders();
    MarkDirty();
}

void UIDesigner::ApplyAssetToSelection(const std::string& runtimePath) {
    if (runtimePath.empty()) return;
    std::string ext = fs::path(runtimePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext == ".pxui") {
        std::string normalized = runtimePath;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        std::string lowerPath = normalized;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (Node* node = PrimarySelection(); node && lowerPath.find("ui/components/") != std::string::npos) {
            node->type = "component";
            node->component = GeneratedScriptPathForRuntimePath(normalized);
            MarkDirty();
            return;
        }
        OpenDocument(runtimePath);
        return;
    }
    if (Node* node = PrimarySelection()) {
        if (RuntimeTypeFromAsset(runtimePath) == "image") {
            node->type = "image";
            node->image = runtimePath;
        } else {
            node->actionTarget = runtimePath;
        }
        MarkDirty();
    }
}

void UIDesigner::FrameSelection() {
    m_pan = ImVec2(0, 0);
    if (const Node* node = PrimarySelection()) {
        m_pan = ImVec2((static_cast<float>(m_canvasW) * 0.5f - (node->x + node->w * 0.5f)) * m_zoom, (static_cast<float>(m_canvasH) * 0.5f - (node->y + node->h * 0.5f)) * m_zoom);
    }
}

void UIDesigner::SelectNode(const std::string& id, bool append) {
    if (!append) {
        m_selectedIds.clear();
    }
    auto it = std::find(m_selectedIds.begin(), m_selectedIds.end(), id);
    if (it != m_selectedIds.end()) {
        if (append) m_selectedIds.erase(it);
        return;
    }
    m_selectedIds.push_back(id);
}

void UIDesigner::ClearSelection() {
    m_selectedIds.clear();
}

void UIDesigner::RecalculateLayerOrders() {
    for (size_t layerIndex = 0; layerIndex < m_layers.size(); ++layerIndex) {
        int order = 0;
        for (Node& node : m_nodes) {
            if (node.layerId == m_layers[layerIndex].id) {
                node.layer = static_cast<int>(layerIndex);
                node.order = order++;
            }
        }
    }
}

void UIDesigner::MoveLayer(int from, int to) {
    if (from < 0 || to < 0 || from >= static_cast<int>(m_layers.size()) || to >= static_cast<int>(m_layers.size()) || from == to) {
        return;
    }
    Layer layer = m_layers[from];
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, std::move(layer));
    RecalculateLayerOrders();
    MarkDirty();
}

void UIDesigner::MoveNodeOrder(Node& node, int direction) {
    node.order += direction;
    node.order = std::max(0, node.order);
}

void UIDesigner::SortNodesForRuntime(std::vector<Node*>& nodes) {
    nodes.clear();
    nodes.reserve(m_nodes.size());
    for (Node& node : m_nodes) {
        nodes.push_back(&node);
    }
    std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) {
        if (a->layer != b->layer) return a->layer < b->layer;
        if (a->order != b->order) return a->order < b->order;
        return a->id < b->id;
    });
}

UIDesigner::Node* UIDesigner::FindNode(const std::string& id) {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

const UIDesigner::Node* UIDesigner::FindNode(const std::string& id) const {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

UIDesigner::Layer* UIDesigner::FindLayer(const std::string& id) {
    auto it = std::find_if(m_layers.begin(), m_layers.end(), [&](const Layer& layer) { return layer.id == id; });
    return it == m_layers.end() ? nullptr : &(*it);
}

const UIDesigner::Layer* UIDesigner::FindLayer(const std::string& id) const {
    auto it = std::find_if(m_layers.begin(), m_layers.end(), [&](const Layer& layer) { return layer.id == id; });
    return it == m_layers.end() ? nullptr : &(*it);
}

bool UIDesigner::IsSelected(const std::string& id) const {
    return std::find(m_selectedIds.begin(), m_selectedIds.end(), id) != m_selectedIds.end();
}

UIDesigner::Node* UIDesigner::PrimarySelection() {
    return m_selectedIds.empty() ? nullptr : FindNode(m_selectedIds.back());
}

const UIDesigner::Node* UIDesigner::PrimarySelection() const {
    return m_selectedIds.empty() ? nullptr : FindNode(m_selectedIds.back());
}

ImRect UIDesigner::NodeRect(const Node& node) const {
    return ImRect(ImVec2(node.x, node.y), ImVec2(node.x + node.w, node.y + node.h));
}

ImVec2 UIDesigner::ScreenToCanvas(const ImVec2& point, const ImVec2& origin, float scale) const {
    return (point - origin) / scale;
}

ImVec2 UIDesigner::CanvasToScreen(const ImVec2& point, const ImVec2& origin, float scale) const {
    return origin + point * scale;
}

std::string UIDesigner::SelectionSummary() const {
    if (m_selectedIds.empty()) {
        return "No selection";
    }
    if (m_selectedIds.size() == 1) {
        if (const Node* node = PrimarySelection()) return node->name + " (" + node->type + ")";
    }
    return std::to_string(m_selectedIds.size()) + " elements";
}

std::vector<std::string> UIDesigner::EnumerateUiDocuments() const {
    std::vector<std::string> documents;
    if (!m_project || !m_project->IsOpen()) {
        return documents;
    }
    const fs::path root = m_project->DataRoot() / "UI";
    if (!fs::exists(root)) {
        return documents;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".pxui") {
            continue;
        }
        documents.push_back((fs::path("UI") / fs::relative(entry.path(), root)).generic_string());
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

std::vector<UIDesigner::GeneratedDocument> UIDesigner::BuildGeneratedDocuments() const {
    std::vector<GeneratedDocument> documents;
    std::unordered_set<std::string> emitted;
    const auto addDocument = [&](const std::string& runtimePath, const Json& document) {
        if (runtimePath.empty() || !emitted.insert(runtimePath).second) {
            return;
        }
        documents.push_back({ "UI Document", fs::path(runtimePath), document.dump(2) + "\n" });
        documents.push_back({ "Generated UI Script", fs::path(GeneratedScriptPathForRuntimePath(runtimePath)), GenerateLuaFromDocument(document, runtimePath) });
    };

    addDocument(CurrentDocumentRuntimePath(), SaveDocument());

    if (m_project && m_project->IsOpen()) {
        for (const std::string& runtimePath : EnumerateUiDocuments()) {
            if (runtimePath == CurrentDocumentRuntimePath()) {
                continue;
            }
            try {
                std::ifstream in(m_project->DataRoot() / fs::path(runtimePath), std::ios::binary);
                addDocument(runtimePath, Json::parse(in));
            } catch (const std::exception& exception) {
                Log("Skipped UI document artifact: " + runtimePath + " (" + exception.what() + ")");
            }
        }
    }
    return documents;
}

std::vector<ExportArtifact> UIDesigner::BuildArtifacts() const {
    std::vector<ExportArtifact> artifacts;
    for (const GeneratedDocument& document : BuildGeneratedDocuments()) {
        artifacts.push_back({ document.label, document.relativePath, document.content });
    }
    return artifacts;
}

std::string UIDesigner::GenerateLua() const {
    return GenerateLuaFromDocument(SaveDocument(), CurrentDocumentRuntimePath());
}

Json UIDesigner::ColorToJson(const ImVec4& color) {
    return Json::array({
        static_cast<int>(std::round(color.x * 255.0f)),
        static_cast<int>(std::round(color.y * 255.0f)),
        static_cast<int>(std::round(color.z * 255.0f)),
        static_cast<int>(std::round(color.w * 255.0f)),
    });
}

ImVec4 UIDesigner::ColorFromJson(const Json& json, ImVec4 fallback) {
    if (!json.is_array() || json.empty()) {
        return fallback;
    }
    return ImVec4(
        json.size() > 0 ? std::clamp(json[0].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.x,
        json.size() > 1 ? std::clamp(json[1].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.y,
        json.size() > 2 ? std::clamp(json[2].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.z,
        json.size() > 3 ? std::clamp(json[3].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.w);
}

std::string UIDesigner::QuoteLua(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else {
            out.push_back(ch);
        }
    }
    out += "\"";
    return out;
}

std::string UIDesigner::SanitizeId(std::string value) {
    value = Trim(value);
    if (value.empty()) value = "node";
    for (char& ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
            ch = '_';
        } else {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return value;
}

std::string UIDesigner::NormalizeUiRuntimePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = Trim(value);
    if (value.empty()) {
        return {};
    }
    if (value.rfind("Data/", 0) == 0) {
        value.erase(0, 5);
    }
    if (value.find('/') == std::string::npos) {
        value = "UI/" + value;
    }
    if (value.rfind("UI/", 0) != 0) {
        value = "UI/" + value;
    }
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (!lower.ends_with(".pxui")) {
        value += ".pxui";
    }
    return value;
}

std::string UIDesigner::GeneratedScriptPathForRuntimePath(std::string runtimePath) {
    std::replace(runtimePath.begin(), runtimePath.end(), '\\', '/');
    if (runtimePath.rfind("Data/", 0) == 0) {
        runtimePath.erase(0, 5);
    }
    if (runtimePath.rfind("UI/", 0) == 0) {
        runtimePath.erase(0, 3);
    }
    const bool component = runtimePath.rfind("components/", 0) == 0;
    if (component) {
        runtimePath.erase(0, std::string("components/").size());
    }
    fs::path output(runtimePath);
    output.replace_extension(".lua");
    return ((component ? fs::path("Scripts/ui/components") : fs::path("Scripts/ui/scenes")) / output).generic_string();
}

std::string UIDesigner::GenerateLuaFromDocument(const Json& document, const std::string& runtimePath) {
    const Json canvas = document.value("canvas", Json::object());
    const Json background = canvas.value("backgroundColor", Json::array({ 0, 0, 0, 255 }));
    const auto colorAt = [](const Json& color, size_t index, int fallback) {
        return color.is_array() && color.size() > index ? color[index].get<int>() : fallback;
    };
    const auto fvalue = [](const Json& item, const char* key, float fallback) {
        return item.contains(key) ? item[key].get<float>() : fallback;
    };
    const auto ivalue = [](const Json& item, const char* key, int fallback) {
        return item.contains(key) ? item[key].get<int>() : fallback;
    };
    const auto bvalue = [](const Json& item, const char* key, bool fallback) {
        return item.contains(key) ? item[key].get<bool>() : fallback;
    };

    std::string normalized = runtimePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool component = normalized.rfind("ui/components/", 0) == 0;

    std::ostringstream lua;
    lua << "-- Generated by PrismatiX UI Designer. Runtime and editor preview load the same document.\n";
    lua << "return " << (component ? "UI.Component" : "UI.Scene") << "({\n";
    lua << "  canvas = { w = " << canvas.value("w", canvas.value("width", 1280)) << ", h = " << canvas.value("h", canvas.value("height", 720)) << " },\n";
    lua << "  background = { color = { " << colorAt(background, 0, 0) << ", " << colorAt(background, 1, 0) << ", " << colorAt(background, 2, 0) << ", " << colorAt(background, 3, 255)
        << " }, image = " << QuoteLua(canvas.value("backgroundImage", canvas.value("image", std::string{}))) << ", alpha = 255 },\n";
    lua << "  layers = {\n";
    for (const Json& layer : document.value("layers", Json::array())) {
        lua << "    { id = " << QuoteLua(layer.value("id", "layer"))
            << ", name = " << QuoteLua(layer.value("name", layer.value("id", "Layer")))
            << ", visible = " << (layer.value("visible", true) ? "true" : "false")
            << ", locked = " << (layer.value("locked", false) ? "true" : "false") << " },\n";
    }
    lua << "  },\n";
    lua << "  nodes = {\n";

    std::vector<Json> nodes;
    for (const Json& node : document.value("nodes", Json::array())) {
        nodes.push_back(node);
    }
    std::sort(nodes.begin(), nodes.end(), [](const Json& a, const Json& b) {
        const int al = a.value("layer", 0);
        const int bl = b.value("layer", 0);
        if (al != bl) return al < bl;
        const int ao = a.value("order", 0);
        const int bo = b.value("order", 0);
        if (ao != bo) return ao < bo;
        return a.value("id", std::string{}) < b.value("id", std::string{});
    });

    for (const Json& node : nodes) {
        const Json bg = node.value("bgColor", Json::array({ 25, 38, 51, 220 }));
        const Json hover = node.value("hoverColor", Json::array({ 40, 71, 87, 240 }));
        const Json border = node.value("borderColor", Json::array({ 118, 144, 184, 120 }));
        const Json hoverBorder = node.value("hoverBorderColor", Json::array({ 255, 214, 143, 220 }));
        const Json text = node.value("textColor", Json::array({ 237, 247, 255, 255 }));
        const Json action = node.value("action", Json::object());
        lua << "    { id = " << QuoteLua(node.value("id", "node"))
            << ", type = " << QuoteLua(node.value("type", "button"))
            << ", name = " << QuoteLua(node.value("name", node.value("id", "Node")))
            << ", layer = " << ivalue(node, "layer", 0)
            << ", order = " << ivalue(node, "order", 0)
            << ", visible = " << (bvalue(node, "visible", true) ? "true" : "false")
            << ", locked = " << (bvalue(node, "locked", false) ? "true" : "false")
            << ", x = " << fvalue(node, "x", 0.0f)
            << ", y = " << fvalue(node, "y", 0.0f)
            << ", w = " << fvalue(node, "w", 220.0f)
            << ", h = " << fvalue(node, "h", 56.0f)
            << ", opacity = " << fvalue(node, "opacity", 255.0f)
            << ", radius = " << fvalue(node, "radius", 8.0f)
            << ", text = " << QuoteLua(node.value("text", std::string{}))
            << ", image = " << QuoteLua(node.value("image", std::string{}))
            << ", hoverImage = " << QuoteLua(node.value("hoverImage", std::string{}))
            << ", component = " << QuoteLua(node.value("component", std::string{}))
            << ", font = " << QuoteLua(node.value("font", "NotoSansTC-Bold.ttf"))
            << ", fontSize = " << ivalue(node, "fontSize", 32)
            << ", bgColor = { " << colorAt(bg, 0, 25) << ", " << colorAt(bg, 1, 38) << ", " << colorAt(bg, 2, 51) << ", " << colorAt(bg, 3, 220) << " }"
            << ", hoverColor = { " << colorAt(hover, 0, 40) << ", " << colorAt(hover, 1, 71) << ", " << colorAt(hover, 2, 87) << ", " << colorAt(hover, 3, 240) << " }"
            << ", borderColor = { " << colorAt(border, 0, 118) << ", " << colorAt(border, 1, 144) << ", " << colorAt(border, 2, 184) << ", " << colorAt(border, 3, 120) << " }"
            << ", hoverBorderColor = { " << colorAt(hoverBorder, 0, 255) << ", " << colorAt(hoverBorder, 1, 214) << ", " << colorAt(hoverBorder, 2, 143) << ", " << colorAt(hoverBorder, 3, 220) << " }"
            << ", borderTopHeight = " << fvalue(node, "borderTopHeight", 0.0f)
            << ", textColor = { " << colorAt(text, 0, 237) << ", " << colorAt(text, 1, 247) << ", " << colorAt(text, 2, 255) << ", " << colorAt(text, 3, 255) << " }"
            << ", textShadow = " << (bvalue(node, "textShadow", false) ? "true" : "false")
            << ", state = " << QuoteLua(node.value("state", "normal"))
            << ", action = { type = " << QuoteLua(action.value("type", "ui.toggle"))
            << ", target = " << QuoteLua(action.value("target", std::string{}))
            << ", argument = " << QuoteLua(action.value("argument", std::string{})) << " }";
        if (node.contains("animations") && node["animations"].is_array() && !node["animations"].empty()) {
            lua << ", animations = {";
            for (const Json& clip : node["animations"]) {
                lua << " { name = " << QuoteLua(clip.value("name", "Animation"))
                    << ", duration = " << clip.value("duration", 0.4f)
                    << ", delay = " << clip.value("delay", 0.0f)
                    << ", easing = " << QuoteLua(clip.value("easing", "outCubic"))
                    << ", loop = " << (clip.value("loop", false) ? "true" : "false")
                    << ", trigger = " << QuoteLua(clip.value("trigger", "scene.enter"))
                    << ", keys = {";
                for (const Json& key : clip.value("keys", Json::array())) {
                    lua << " { time = " << key.value("time", 0.0f)
                        << ", property = " << QuoteLua(key.value("property", "opacity"))
                        << ", value = " << key.value("value", 255.0f) << " },";
                }
                lua << " } },";
            }
            lua << " }";
        }
        lua << " },\n";
    }
    lua << "  },\n";
    lua << "})\n";
    return lua.str();
}

void UIDesigner::RenderTextRow(const char* label, std::string& value) {
    if (ImGui::InputText(label, &value)) {
        MarkDirty();
    }
}

void UIDesigner::RenderNodeTypeCombo(Node& node) {
    static const std::array<const char*, 11> types{ "image", "text", "button", "frame", "group", "stack", "component", "dialogue_box", "choice_list", "backlog", "custom" };
    if (ImGui::BeginCombo("Type", node.type.c_str())) {
        for (const char* type : types) {
            const bool selected = node.type == type;
            if (ImGui::Selectable(type, selected)) {
                node.type = type;
                MarkDirty();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void UIDesigner::RenderActionTypeCombo(Node& node) {
    static const std::array<const char*, 9> actionTypes{
        "scene.start", "scene.switch", "game.exit", "save.open", "save.slot", "ui.toggle", "ui.set_state", "audio.play", "script.call",
    };
    if (ImGui::BeginCombo("Action", node.actionType.c_str())) {
        for (const char* type : actionTypes) {
            const bool selected = node.actionType == type;
            if (ImGui::Selectable(type, selected)) {
                node.actionType = type;
                MarkDirty();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void UIDesigner::MarkDirty() {
    m_dirty = true;
    ++m_revision;
}

void UIDesigner::Log(const std::string& message) const {
    if (m_log) {
        m_log(message);
    }
}

}
