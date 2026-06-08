#define IMGUI_DEFINE_MATH_OPERATORS

#include "NodeGraphEditor.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <widgets.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace ed = ax::NodeEditor;
namespace fs = std::filesystem;
using ax::Drawing::IconType;

namespace px::editor {

namespace {

constexpr float kNodeRounding = 12.0f;
constexpr float kNodeContentWidth = 540.0f;
constexpr float kInputColumnWidth = 318.0f;
constexpr float kOutputColumnWidth = 182.0f;
constexpr float kOutputAlignWidth = 158.0f;
constexpr float kCompactParameterWidth = 176.0f;
constexpr float kPinSize = 18.0f;
constexpr float kPinLabelSpacing = 6.0f;
constexpr float kHeaderTextureScale = 6.0f;
constexpr const char* kResourcePayload = "PX_RESOURCE_PATH";

std::string TrimCopy(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) ++start;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return std::string(value.substr(start, end - start));
}

std::string ExtractArg(std::string_view body, std::string_view key) {
    const std::string needle = std::string(key) + "=";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
    }
    if (pos >= body.size()) {
        return {};
    }
    if (body[pos] == '"' || body[pos] == '\'') {
        const char quote = body[pos++];
        std::string value;
        bool escape = false;
        while (pos < body.size()) {
            const char ch = body[pos++];
            if (!escape && ch == quote) {
                break;
            }
            if (!escape && ch == '\\') {
                escape = true;
                continue;
            }
            value.push_back(ch);
            escape = false;
        }
        return value;
    }
    const size_t start = pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) == 0 && body[pos] != ']') {
        ++pos;
    }
    return std::string(body.substr(start, pos - start));
}

std::string CommandArgs(std::string_view command) {
    if (command.size() < 2 || command.front() != '[' || command.back() != ']') {
        return {};
    }
    std::string_view body = command.substr(1, command.size() - 2);
    size_t pos = 0;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) == 0) ++pos;
    return TrimCopy(body.substr(pos));
}

struct PDSArg {
    std::string key;
    std::string value;
};

std::vector<PDSArg> ParsePDSArgs(std::string_view args) {
    std::vector<PDSArg> parsed;
    size_t pos = 0;
    while (pos < args.size()) {
        while (pos < args.size() && std::isspace(static_cast<unsigned char>(args[pos])) != 0) ++pos;
        if (pos >= args.size()) break;
        const size_t keyStart = pos;
        while (pos < args.size() && args[pos] != '=' && std::isspace(static_cast<unsigned char>(args[pos])) == 0) ++pos;
        std::string key = std::string(args.substr(keyStart, pos - keyStart));
        while (pos < args.size() && std::isspace(static_cast<unsigned char>(args[pos])) != 0) ++pos;
        if (pos >= args.size() || args[pos] != '=') {
            break;
        }
        ++pos;
        while (pos < args.size() && std::isspace(static_cast<unsigned char>(args[pos])) != 0) ++pos;
        std::string value;
        if (pos < args.size() && (args[pos] == '"' || args[pos] == '\'')) {
            const char quote = args[pos++];
            bool escape = false;
            while (pos < args.size()) {
                const char ch = args[pos++];
                if (!escape && ch == quote) break;
                if (!escape && ch == '\\') {
                    escape = true;
                    continue;
                }
                value.push_back(ch);
                escape = false;
            }
        } else {
            const size_t valueStart = pos;
            while (pos < args.size() && std::isspace(static_cast<unsigned char>(args[pos])) == 0) ++pos;
            value = std::string(args.substr(valueStart, pos - valueStart));
        }
        if (!key.empty()) {
            parsed.push_back({ std::move(key), std::move(value) });
        }
    }
    return parsed;
}

std::string QuotePDSValue(std::string_view value) {
    std::string out = "\"";
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out.push_back(ch); break;
        }
    }
    out += "\"";
    return out;
}

std::string TextFormatArgs(std::string_view args) {
    std::ostringstream out;
    bool first = true;
    for (const PDSArg& arg : ParsePDSArgs(args)) {
        std::string lower = arg.key;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lower == "speaker" || lower == "name" || lower == "text" || lower == "content") {
            continue;
        }
        if (!first) out << ' ';
        out << arg.key << '=' << QuotePDSValue(arg.value);
        first = false;
    }
    return out.str();
}

std::vector<std::string> SplitTextLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

std::string JoinTextLines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out << '\n';
        }
        out << lines[i];
    }
    return out.str();
}

ImVec4 ColorFromPDSCsv(std::string_view value, ImVec4 fallback) {
    std::string text(value);
    std::replace(text.begin(), text.end(), ';', ',');
    std::istringstream in(text);
    std::string part;
    std::array<float, 4> channels{ fallback.x, fallback.y, fallback.z, fallback.w };
    size_t index = 0;
    while (index < channels.size() && std::getline(in, part, ',')) {
        try {
            channels[index++] = std::clamp(std::stof(TrimCopy(part)) / 255.0f, 0.0f, 1.0f);
        } catch (...) {
            return fallback;
        }
    }
    return ImVec4(channels[0], channels[1], channels[2], channels[3]);
}

std::string ColorToPDSCsv(const ImVec4& color) {
    std::ostringstream out;
    out << static_cast<int>(std::round(std::clamp(color.x, 0.0f, 1.0f) * 255.0f)) << ","
        << static_cast<int>(std::round(std::clamp(color.y, 0.0f, 1.0f) * 255.0f)) << ","
        << static_cast<int>(std::round(std::clamp(color.z, 0.0f, 1.0f) * 255.0f));
    return out.str();
}

bool SameColor(const ImVec4& a, const ImVec4& b) {
    return std::abs(a.x - b.x) < 0.002f && std::abs(a.y - b.y) < 0.002f && std::abs(a.z - b.z) < 0.002f && std::abs(a.w - b.w) < 0.002f;
}

int IntFromString(const std::string& value, int fallback) {
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

Json ColorToJson(const ImVec4& color) {
    return Json::array({
        static_cast<int>(std::round(color.x * 255.0f)),
        static_cast<int>(std::round(color.y * 255.0f)),
        static_cast<int>(std::round(color.z * 255.0f)),
        static_cast<int>(std::round(color.w * 255.0f)),
    });
}

ImVec4 ColorFromJson(const Json& json, ImVec4 fallback) {
    if (!json.is_array() || json.empty()) {
        return fallback;
    }
    return ImVec4(
        json.size() > 0 ? std::clamp(json[0].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.x,
        json.size() > 1 ? std::clamp(json[1].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.y,
        json.size() > 2 ? std::clamp(json[2].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.z,
        json.size() > 3 ? std::clamp(json[3].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.w);
}

}

NodeGraphEditor::NodeGraphEditor(GraphKind kind, LogSink log) : m_kind(kind), m_log(std::move(log)) {
    if (m_kind == GraphKind::PDSDialogue) {
        m_documentRuntimePath = "Script/chapter1.pds";
        m_documentPathInput = m_documentRuntimePath;
    }
    BuildLibrary();
}

NodeGraphEditor::~NodeGraphEditor() {
    if (m_context) {
        ed::DestroyEditor(m_context);
        m_context = nullptr;
    }
}

void NodeGraphEditor::EnsureContext() {
    if (m_context) {
        return;
    }
    ed::Config config;
    config.SettingsFile = nullptr;
    m_context = ed::CreateEditor(&config);
}

void NodeGraphEditor::SetProject(const ProjectContext* context) {
    if (m_project == context && context && m_loaded) {
        return;
    }
    m_project = context;
    m_loaded = false;
    LoadOrCreate();
}

void NodeGraphEditor::SetSelectedResourceCallback(SelectedResourceCallback callback) {
    m_selectedResource = std::move(callback);
}

void NodeGraphEditor::SetHeaderTexture(ImTextureID texture, int width, int height) {
    m_headerTexture = texture;
    m_headerTextureWidth = width;
    m_headerTextureHeight = height;
}

std::string NodeGraphEditor::Title() const {
    return m_kind == GraphKind::PDSDialogue ? "PDS Dialogue Graph" : "Gameloop Graph";
}

fs::path NodeGraphEditor::DocumentPath() const {
    if (!m_project || !m_project->IsOpen()) {
        return {};
    }
    if (m_kind == GraphKind::PDSDialogue) {
        return m_project->DataRoot() / fs::path(m_documentRuntimePath.empty() ? "Script/chapter1.pds" : m_documentRuntimePath);
    }
    const fs::path graphRoot = m_project->DataRoot() / "Graphs";
    return graphRoot / "gameloop.pxgraph";
}

std::string NodeGraphEditor::CurrentRuntimePath() const {
    return m_kind == GraphKind::PDSDialogue ? (m_documentRuntimePath.empty() ? "Script/chapter1.pds" : m_documentRuntimePath) : "Scripts/entrypoint.lua";
}

void NodeGraphEditor::BuildLibrary() {
    m_library.clear();

    const auto paramString = [](std::string key, std::string label, std::string value, ParamType type = ParamType::String, bool multiline = false) {
        Parameter p;
        p.key = std::move(key);
        p.label = std::move(label);
        p.stringValue = std::move(value);
        p.type = type;
        p.multiline = multiline;
        return p;
    };
    const auto paramInt = [](std::string key, std::string label, int value, int minValue, int maxValue) {
        Parameter p;
        p.key = std::move(key);
        p.label = std::move(label);
        p.type = ParamType::Int;
        p.intValue = value;
        p.minValue = static_cast<float>(minValue);
        p.maxValue = static_cast<float>(maxValue);
        return p;
    };
    const auto paramFloat = [](std::string key, std::string label, float value, float minValue, float maxValue) {
        Parameter p;
        p.key = std::move(key);
        p.label = std::move(label);
        p.type = ParamType::Float;
        p.floatValue = value;
        p.minValue = minValue;
        p.maxValue = maxValue;
        return p;
    };
    const auto paramColor = [](std::string key, std::string label, ImVec4 value) {
        Parameter p;
        p.key = std::move(key);
        p.label = std::move(label);
        p.type = ParamType::Color;
        p.colorValue = value;
        return p;
    };
    const auto paramBool = [](std::string key, std::string label, bool value) {
        Parameter p;
        p.key = std::move(key);
        p.label = std::move(label);
        p.type = ParamType::Bool;
        p.boolValue = value;
        return p;
    };
    const auto paramOption = [](std::string key, std::string label, std::string value, std::vector<std::string> options) {
        Parameter p;
        p.key = std::move(key);
        p.label = std::move(label);
        p.type = ParamType::Option;
        p.stringValue = std::move(value);
        p.options = std::move(options);
        return p;
    };

    const auto in = [](std::string label, PinType type = PinType::Flow, std::string key = {}) {
        return PinTemplate{ std::move(label), type, true, std::move(key) };
    };
    const auto out = [](std::string label, PinType type = PinType::Flow, std::string key = {}) {
        return PinTemplate{ std::move(label), type, false, std::move(key) };
    };

    if (m_kind == GraphKind::PDSDialogue) {
        m_library = {
            { "pds_start", "Start", "Timeline", "Entry for this .pds document.", ImColor(80, 184, 255), {}, { out("Begin") }, { paramString("title", "Chapter Title", "第一章") } },
            { "chapter", "Chapter", "Timeline", "Emits a chapter heading.", ImColor(80, 184, 255), { in("In") }, { out("Out") }, { paramString("title", "Title", "第一章") } },
            { "background", "Background", "Stage", "Sets the current background image.", ImColor(67, 186, 255), { in("In"), in("Image", PinType::Asset, "file") }, { out("Out") }, { paramString("file", "Image", "sky.png", ParamType::Asset) } },
            { "bgm", "Play BGM", "Audio", "Starts background music.", ImColor(255, 126, 95), { in("In"), in("BGM", PinType::Asset, "file") }, { out("Out") }, { paramString("file", "Audio", "bgm1.mp3", ParamType::Asset) } },
            { "character", "Character", "Stage", "Places a character sprite.", ImColor(255, 194, 97), { in("In") }, { out("Out") }, { paramString("id", "Character", "girl"), paramString("expression", "Expression", "d"), paramString("pos", "Position", "2") } },
            { "dialogue", "Dialogue Text", "Dialogue", "Shows one or more speaker lines with the same text format.", ImColor(160, 132, 255), { in("In"), in("Text", PinType::String, "text") }, { out("Out") }, { paramString("speaker", "Speaker", "伊莉雅"), paramString("character", "Character", ""), paramColor("color", "Color", ImVec4(1, 1, 1, 1)), paramColor("outline", "Outline", ImVec4(0, 0, 0, 1)), paramInt("speed", "Speed", 40, 0, 240), paramOption("effect", "Effect", "none", { "none", "shake", "pulse" }), paramString("text", "Text", "新的對話", ParamType::String, true) } },
            { "choice", "Choice", "Dialogue", "Adds a choice option.", ImColor(103, 219, 177), { in("In") }, { out("Out") }, { paramString("text", "Text", "選項"), paramString("target", "Target", "*end") } },
            { "label", "Label", "Flow", "Defines a jump target.", ImColor(103, 219, 177), { in("In") }, { out("Out") }, { paramString("name", "Label", "end") } },
            { "jump", "Jump", "Flow", "Jumps to a label or another script.", ImColor(103, 219, 177), { in("In"), in("Target", PinType::String, "target") }, { out("Out") }, { paramString("target", "Target", "*end") } },
            { "variable", "Variable", "Logic", "Sets or changes a VN variable.", ImColor(232, 178, 92), { in("In") }, { out("Out") }, { paramString("var", "Name", "affection"), paramOption("op", "Operation", "set", { "set", "add" }), paramString("value", "Value", "0") } },
            { "lua", "Lua Command", "Logic", "Runs a Lua hook command.", ImColor(232, 178, 92), { in("In") }, { out("Out") }, { paramString("fn", "Function", "PXEditorTransition"), paramString("args", "Arguments", "style=\"fade\" speed=\"10\"") } },
            { "transition", "Transition", "Stage", "Runs the built-in editor transition hook.", ImColor(91, 209, 244), { in("In") }, { out("Out") }, { paramOption("style", "Style", "fade", { "fade", "dissolve", "wipe" }), paramFloat("speed", "Speed", 10.0f, 1.0f, 60.0f) } },
            { "animate_actor", "Animate Actor", "Stage", "Runs the built-in actor animation hook.", ImColor(91, 209, 244), { in("In") }, { out("Out") }, { paramString("target", "Target", "girl"), paramFloat("duration", "Duration", 24, 1, 240), paramFloat("move_x", "Move X", 0, -1280, 1280) } },
            { "spawn_ui", "Spawn UI", "UI", "Mounts a generated UI script.", ImColor(88, 230, 184), { in("In"), in("UI", PinType::Asset, "ui") }, { out("Out") }, { paramString("ui", "UI Script", "Scripts/ui/generated/title_menu.lua", ParamType::Asset) } },
            { "stop_bgm", "Stop BGM", "Audio", "Stops background music.", ImColor(255, 126, 95), { in("In") }, { out("Out") }, {} },
            { "raw", "Raw PDS", "Advanced", "Writes a raw PDS command or content line.", ImColor(142, 152, 170), { in("In") }, { out("Out") }, { paramString("line", "Line", "[wait]") } },
            { "string", "String", "Literals", "Reusable text value.", ImColor(160, 132, 255), {}, { out("Value", PinType::String, "value") }, { paramString("value", "Value", "text") } },
            { "asset", "Asset", "Literals", "Reusable asset reference.", ImColor(67, 186, 255), {}, { out("Path", PinType::Asset, "path") }, { paramString("path", "Path", "sky.png", ParamType::Asset) } },
            { "bool", "Bool", "Literals", "Reusable boolean.", ImColor(255, 104, 104), {}, { out("Value", PinType::Bool, "value") }, { paramBool("value", "Value", true) } },
        };
    } else {
        m_library = {
            { "boot", "Boot Runtime", "Boot", "Defines game title and typography.", ImColor(80, 184, 255), {}, { out("Begin") }, { paramString("game_title", "Game Title", "PrismatiX Demo"), paramString("font", "Font", "NotoSansTC-Bold.ttf", ParamType::Asset), paramInt("font_size", "Font Size", 32, 14, 96) } },
            { "configure", "Configure Systems", "Boot", "Sets transition and notification defaults.", ImColor(88, 230, 184), { in("In") }, { out("Out") }, { paramOption("transition", "Transition", "dissolve", { "dissolve", "fade", "wipe" }), paramInt("speed", "Speed", 10, 1, 60), paramInt("notification", "Notification Font", 20, 10, 48) } },
            { "load_modules", "Load Modules", "Boot", "Loads shared Lua modules and UI runtime.", ImColor(80, 184, 255), { in("In") }, { out("Out") }, {} },
            { "splash", "Run Splash", "Boot", "Runs engine and game splash scripts.", ImColor(255, 194, 97), { in("In") }, { out("Out") }, { paramString("engine", "Engine Splash", "Scripts/engine_splash.lua", ParamType::Asset), paramString("game", "Game Splash", "Scripts/splash.lua", ParamType::Asset) } },
            { "switch_scene", "Switch Scene", "Boot", "Starts the first scene and PDS script.", ImColor(160, 132, 255), { in("In") }, { out("Out") }, { paramString("scene", "Scene Module", "Scripts/scenes/title_scene.lua", ParamType::Asset), paramString("ctor", "Scene Constructor", "TitleScene"), paramString("pds", "Start PDS", "Script/chapter1.pds", ParamType::Asset) } },
            { "loop_start", "Loop Start", "Loop", "Begins the Engine.IsRunning loop.", ImColor(103, 219, 177), { in("In") }, { out("Loop") }, {} },
            { "handle_events", "Handle Events", "Loop", "Polls input and window events.", ImColor(103, 219, 177), { in("In") }, { out("Out") }, {} },
            { "update_fx", "Update FX", "Loop", "Updates screen effects and camera offset.", ImColor(91, 209, 244), { in("In") }, { out("Out") }, {} },
            { "update_scene", "Update Scene", "Loop", "Updates the active scene.", ImColor(91, 209, 244), { in("In") }, { out("Out") }, {} },
            { "render_scene", "Render Scene", "Render", "Renders scene content.", ImColor(67, 186, 255), { in("In") }, { out("Out") }, {} },
            { "render_ui", "Render UI", "Render", "Renders runtime notifications and UI overlays.", ImColor(67, 186, 255), { in("In") }, { out("Out") }, {} },
            { "render_transition", "Render Transition", "Render", "Draws transition overlays.", ImColor(67, 186, 255), { in("In") }, { out("Out") }, {} },
            { "present", "Present", "Render", "Presents the frame.", ImColor(67, 186, 255), { in("In") }, { out("Out") }, {} },
        };
    }

    if (m_kind == GraphKind::PDSDialogue) {
        for (const CustomCommandDef& cmd : m_customCommands) {
            NodeTemplate tpl;
            tpl.type = cmd.name;
            tpl.title = cmd.name;
            tpl.category = cmd.category.empty() ? "Custom" : cmd.category;
            tpl.description = cmd.description.empty()
                                  ? ("Custom Lua command (" + cmd.sourceFile + ")")
                                  : cmd.description;
            tpl.accent = ImColor(214, 122, 255);
            tpl.inputs = { in("In") };
            tpl.outputs = { out("Out") };
            for (const CustomCommandParam& p : cmd.params) {
                tpl.defaults.push_back(paramString(p.key, p.key, p.defaultValue));
            }
            m_library.push_back(std::move(tpl));
        }
    }
}

const CustomCommandDef* NodeGraphEditor::FindCustomCommand(std::string_view type) const {
    const std::string lower = Lower(std::string(type));
    for (const CustomCommandDef& c : m_customCommands) {
        if (c.name == type || Lower(c.name) == lower) return &c;
    }
    return nullptr;
}

void NodeGraphEditor::SetCustomCommands(std::vector<CustomCommandDef> commands) {
    m_customCommands = std::move(commands);
    BuildLibrary();
}

void NodeGraphEditor::LoadOrCreate() {
    m_nodes.clear();
    m_links.clear();
    m_nextId = 1;
    m_selectedNodeId = 0;
    m_selectedLinkId = 0;
    m_dirty = false;

    const fs::path path = DocumentPath();
    if (path.empty()) {
        return;
    }

    if (m_kind == GraphKind::PDSDialogue) {
        if (ImportPDSScript(path)) {
            m_dirty = false;
            m_loaded = true;
            m_navigateCountdown = 3;
            Log("Loaded PDS document: " + path.string());
            return;
        }
        SeedDefaultGraph();
        Save();
        m_loaded = true;
        m_navigateCountdown = 3;
        return;
    }

    if (fs::exists(path)) {
        try {
            std::ifstream in(path, std::ios::binary);
            LoadGraph(Json::parse(in));
            m_loaded = true;
            m_navigateCountdown = 3;
            return;
        } catch (const std::exception& exception) {
            Log(std::string("Graph load failed, creating default: ") + exception.what());
        }
    }

    SeedDefaultGraph();
    Save();
    m_loaded = true;
    m_navigateCountdown = 3;
}

void NodeGraphEditor::Reload() {
    m_loaded = false;
    LoadOrCreate();
}

bool NodeGraphEditor::OpenDocument(const std::string& runtimePath) {
    if (m_kind != GraphKind::PDSDialogue || !m_project || !m_project->IsOpen()) {
        return false;
    }
    const std::string normalized = NormalizePDSRuntimePath(runtimePath);
    if (normalized.empty()) {
        return false;
    }
    if (m_dirty) {
        Save();
    }
    m_documentRuntimePath = normalized;
    m_documentPathInput = normalized;
    m_loaded = false;
    LoadOrCreate();
    return true;
}

bool NodeGraphEditor::NewDocument(const std::string& runtimePath) {
    if (m_kind != GraphKind::PDSDialogue || !m_project || !m_project->IsOpen()) {
        return false;
    }
    const std::string normalized = NormalizePDSRuntimePath(runtimePath);
    if (normalized.empty()) {
        return false;
    }
    if (m_dirty) {
        Save();
    }
    m_documentRuntimePath = normalized;
    m_documentPathInput = normalized;
    SeedDefaultGraph();
    Save();
    m_loaded = true;
    m_navigateCountdown = 3;
    return true;
}

void NodeGraphEditor::LoadGraph(const Json& json) {
    m_nodes.clear();
    m_links.clear();
    m_nextId = json.value("nextId", 1);

    for (const Json& item : json.value("nodes", Json::array())) {
        Node node;
        node.id = item.value("id", m_nextId++);
        node.type = item.value("type", "");
        const NodeTemplate* definition = FindTemplate(node.type);
        if (!definition) {
            continue;
        }
        node.parameters = definition->defaults;
        if (item.contains("position") && item["position"].is_array() && item["position"].size() >= 2) {
            node.position = ImVec2(item["position"][0].get<float>(), item["position"][1].get<float>());
            node.positionInitialized = true;
        }

        int owner = node.id;
        for (const PinTemplate& pinTemplate : definition->inputs) {
            node.inputs.push_back(Pin{ m_nextId++, owner, pinTemplate.label, pinTemplate.type, true, pinTemplate.parameterKey });
        }
        for (const PinTemplate& pinTemplate : definition->outputs) {
            node.outputs.push_back(Pin{ m_nextId++, owner, pinTemplate.label, pinTemplate.type, false, pinTemplate.parameterKey });
        }

        if (item.contains("params")) {
            for (Parameter& parameter : node.parameters) {
                if (!item["params"].contains(parameter.key)) {
                    continue;
                }
                const Json& value = item["params"][parameter.key];
                switch (parameter.type) {
                    case ParamType::Bool:
                        parameter.boolValue = value.get<bool>();
                        break;
                    case ParamType::Int:
                        parameter.intValue = value.get<int>();
                        break;
                    case ParamType::Float:
                        parameter.floatValue = value.get<float>();
                        break;
                    case ParamType::Color:
                        parameter.colorValue = ColorFromJson(value, parameter.colorValue);
                        break;
                    case ParamType::String:
                    case ParamType::Asset:
                    case ParamType::Option:
                        parameter.stringValue = value.get<std::string>();
                        break;
                }
            }
        }
        m_nodes.push_back(std::move(node));
    }

    const auto pinIdByNodeAndIndex = [&](int nodeId, bool input, int index) {
        if (const Node* node = FindNode(nodeId)) {
            const auto& pins = input ? node->inputs : node->outputs;
            if (index >= 0 && index < static_cast<int>(pins.size())) {
                return pins[index].id;
            }
        }
        return 0;
    };

    for (const Json& item : json.value("links", Json::array())) {
        Link link;
        link.id = item.value("id", m_nextId++);
        link.startPinId = item.value("startPinId", 0);
        link.endPinId = item.value("endPinId", 0);
        if (link.startPinId == 0 && item.contains("from")) {
            link.startPinId = pinIdByNodeAndIndex(item["from"].value("node", 0), false, item["from"].value("pin", 0));
        }
        if (link.endPinId == 0 && item.contains("to")) {
            link.endPinId = pinIdByNodeAndIndex(item["to"].value("node", 0), true, item["to"].value("pin", 0));
        }
        if (FindPin(link.startPinId) && FindPin(link.endPinId)) {
            m_links.push_back(link);
            m_nextId = std::max(m_nextId, link.id + 1);
        }
    }
    m_dirty = false;
}

Json NodeGraphEditor::SaveGraph() const {
    Json graph;
    graph["version"] = 1;
    graph["graphKind"] = m_kind == GraphKind::PDSDialogue ? "pdsDialogue" : "gameloop";
    graph["output"] = CurrentRuntimePath();
    graph["nextId"] = m_nextId;
    graph["comments"] = Json::array();
    graph["groups"] = Json::array();
    graph["nodes"] = Json::array();
    graph["links"] = Json::array();

    for (const Node& node : m_nodes) {
        Json item;
        item["id"] = node.id;
        item["type"] = node.type;
        item["position"] = Json::array({ node.position.x, node.position.y });
        item["params"] = Json::object();
        for (const Parameter& parameter : node.parameters) {
            switch (parameter.type) {
                case ParamType::Bool:
                    item["params"][parameter.key] = parameter.boolValue;
                    break;
                case ParamType::Int:
                    item["params"][parameter.key] = parameter.intValue;
                    break;
                case ParamType::Float:
                    item["params"][parameter.key] = parameter.floatValue;
                    break;
                case ParamType::Color:
                    item["params"][parameter.key] = ColorToJson(parameter.colorValue);
                    break;
                case ParamType::String:
                case ParamType::Asset:
                case ParamType::Option:
                    item["params"][parameter.key] = parameter.stringValue;
                    break;
            }
        }
        graph["nodes"].push_back(std::move(item));
    }

    const auto pinIndex = [&](int pinId) {
        for (const Node& node : m_nodes) {
            for (size_t i = 0; i < node.inputs.size(); ++i) {
                if (node.inputs[i].id == pinId) {
                    return Json{ { "node", node.id }, { "pin", static_cast<int>(i) } };
                }
            }
            for (size_t i = 0; i < node.outputs.size(); ++i) {
                if (node.outputs[i].id == pinId) {
                    return Json{ { "node", node.id }, { "pin", static_cast<int>(i) } };
                }
            }
        }
        return Json{};
    };

    for (const Link& link : m_links) {
        graph["links"].push_back(Json{
            { "id", link.id },
            { "startPinId", link.startPinId },
            { "endPinId", link.endPinId },
            { "from", pinIndex(link.startPinId) },
            { "to", pinIndex(link.endPinId) },
        });
    }
    return graph;
}

void NodeGraphEditor::Save() {
    const fs::path path = DocumentPath();
    if (path.empty()) {
        return;
    }
    UpdateNodePositions();
    fs::create_directories(path.parent_path());
    try {
        std::ofstream out(path, std::ios::binary);
        if (m_kind == GraphKind::PDSDialogue) {
            out << Compile();
        } else {
            out << std::setw(2) << SaveGraph() << "\n";
        }
        m_dirty = false;
        m_loaded = true;
        Log("Saved " + std::string(m_kind == GraphKind::PDSDialogue ? "PDS document" : Title()) + ": " + path.string());
    } catch (const std::exception& exception) {
        Log(std::string("Save failed: ") + exception.what());
    }
}

void NodeGraphEditor::SeedDefaultGraph() {
    m_nodes.clear();
    m_links.clear();
    m_nextId = 1;
    if (m_kind == GraphKind::PDSDialogue) {
        std::vector<std::string> types{ "pds_start", "chapter", "background", "bgm", "character", "dialogue", "choice", "label", "dialogue", "jump", "stop_bgm" };
        for (size_t i = 0; i < types.size(); ++i) {
            AddNode(types[i], ImVec2(static_cast<float>(i) * 280.0f, (i % 2 == 0) ? 0.0f : 170.0f));
        }
    } else {
        std::vector<std::string> types{ "boot", "configure", "load_modules", "splash", "switch_scene", "loop_start", "handle_events", "update_fx", "update_scene", "render_scene", "render_ui", "render_transition", "present" };
        for (size_t i = 0; i < types.size(); ++i) {
            AddNode(types[i], ImVec2(static_cast<float>(i % 5) * 330.0f, static_cast<float>(i / 5) * 220.0f));
        }
    }
    CreateDefaultLinkChain();
    m_dirty = true;
}

bool NodeGraphEditor::ImportPDSScript(const fs::path& path) {
    if (!fs::exists(path)) {
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    m_nodes.clear();
    m_links.clear();
    m_nextId = 1;
    Node* start = AddNode("pds_start", ImVec2(0, 0));
    size_t visualIndex = 1;
    std::string pendingTextSpeaker;
    std::string pendingTextCharacter;
    ImVec4 pendingTextColor = ImVec4(1, 1, 1, 1);
    ImVec4 pendingOutlineColor = ImVec4(0, 0, 0, 1);
    int pendingTextSpeed = 40;
    std::string pendingTextEffect = "none";
    std::string pendingTextSignature;
    std::string activeTextSpeaker;
    std::string activeTextSignature;
    int activeDialogueId = 0;
    ImVec2 pendingNodePos(0, 0);
    bool hasPendingNodePos = false;

    const auto refreshPendingSignature = [&]() {
        std::ostringstream sig;
        sig << pendingTextSpeaker << "|"
            << pendingTextCharacter << "|"
            << ColorToPDSCsv(pendingTextColor) << "|"
            << ColorToPDSCsv(pendingOutlineColor) << "|"
            << pendingTextSpeed << "|"
            << pendingTextEffect;
        pendingTextSignature = sig.str();
    };
    refreshPendingSignature();

    const auto appendDialogueText = [&](const std::string& text) {
        if (text.empty()) {
            return;
        }

        Node* target = nullptr;
        if (activeDialogueId != 0 && activeTextSpeaker == pendingTextSpeaker && activeTextSignature == pendingTextSignature) {
            target = FindNode(activeDialogueId);
        }

        if (!target) {
            target = AddNode("dialogue", ImVec2(static_cast<float>(visualIndex) * 280.0f, 0));
            if (!target) {
                return;
            }
            if (hasPendingNodePos) {
                target->position = pendingNodePos;
                target->positionInitialized = true;
                hasPendingNodePos = false;
            }
            activeDialogueId = target->id;
            activeTextSpeaker = pendingTextSpeaker;
            activeTextSignature = pendingTextSignature;
            if (Parameter* p = FindParameter(*target, "speaker")) p->stringValue = pendingTextSpeaker;
            if (Parameter* p = FindParameter(*target, "character")) p->stringValue = pendingTextCharacter;
            if (Parameter* p = FindParameter(*target, "color")) p->colorValue = pendingTextColor;
            if (Parameter* p = FindParameter(*target, "outline")) p->colorValue = pendingOutlineColor;
            if (Parameter* p = FindParameter(*target, "speed")) p->intValue = pendingTextSpeed;
            if (Parameter* p = FindParameter(*target, "effect")) p->stringValue = pendingTextEffect.empty() ? "none" : pendingTextEffect;
            if (Parameter* p = FindParameter(*target, "text")) p->stringValue = text;
            ++visualIndex;
            return;
        }

        if (Parameter* p = FindParameter(*target, "text")) {
            if (!p->stringValue.empty()) {
                p->stringValue += "\n";
            }
            p->stringValue += text;
        }
    };

    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.rfind("//", 0) == 0) {
            if (trimmed.rfind("//@node", 0) == 0) {
                pendingNodePos = ImVec2(static_cast<float>(IntFromString(ExtractArg(trimmed, "x"), 0)),
                                        static_cast<float>(IntFromString(ExtractArg(trimmed, "y"), 0)));
                hasPendingNodePos = true;
            }
            continue;
        }
        Node* created = nullptr;
        if (trimmed.starts_with("#")) {
            created = AddNode("chapter", ImVec2(static_cast<float>(visualIndex) * 280.0f, 0));
            if (Parameter* p = FindParameter(*created, "title")) p->stringValue = Trim(std::string_view(trimmed).substr(1));
        } else if (trimmed.starts_with("*")) {
            created = AddNode("label", ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
            if (Parameter* p = FindParameter(*created, "name")) p->stringValue = Trim(std::string_view(trimmed).substr(1));
        } else if (trimmed.starts_with("[") && trimmed.ends_with("]")) {
            const size_t separator = trimmed.find_first_of(" \t]");
            const size_t nameEnd = separator == std::string::npos ? trimmed.size() - 1 : separator;
            const std::string name = Lower(Trim(std::string_view(trimmed).substr(1, nameEnd > 0 ? nameEnd - 1 : 0)));
            if (name == "bg") {
                created = AddNode("background", ImVec2(static_cast<float>(visualIndex) * 280.0f, 0));
                if (Parameter* p = FindParameter(*created, "file")) p->stringValue = ExtractArg(trimmed, "file");
            } else if (name == "bgm") {
                created = AddNode("bgm", ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
                if (Parameter* p = FindParameter(*created, "file")) p->stringValue = ExtractArg(trimmed, "file");
            } else if (name == "char") {
                created = AddNode("character", ImVec2(static_cast<float>(visualIndex) * 280.0f, 0));
                if (Parameter* p = FindParameter(*created, "id")) p->stringValue = ExtractArg(trimmed, "id");
                if (Parameter* p = FindParameter(*created, "expression")) p->stringValue = ExtractArg(trimmed, "expression");
                if (Parameter* p = FindParameter(*created, "pos")) p->stringValue = ExtractArg(trimmed, "pos");
            } else if (name == "text") {
                pendingTextSpeaker = ExtractArg(trimmed, "speaker");
                if (pendingTextSpeaker.empty()) {
                    pendingTextSpeaker = ExtractArg(trimmed, "name");
                }
                const std::string args = CommandArgs(trimmed);
                pendingTextCharacter = ExtractArg(args, "char");
                pendingTextColor = ColorFromPDSCsv(ExtractArg(args, "color"), ImVec4(1, 1, 1, 1));
                pendingOutlineColor = ColorFromPDSCsv(ExtractArg(args, "outline"), ColorFromPDSCsv(ExtractArg(args, "outlineColor"), ImVec4(0, 0, 0, 1)));
                pendingTextSpeed = IntFromString(ExtractArg(args, "speed"), 40);
                pendingTextEffect = ExtractArg(args, "effect");
                if (pendingTextEffect.empty()) {
                    pendingTextEffect = "none";
                }
                refreshPendingSignature();
                std::string inlineText = ExtractArg(trimmed, "content");
                if (inlineText.empty()) {
                    inlineText = ExtractArg(trimmed, "text");
                }
                appendDialogueText(inlineText);
                continue;
            } else if (name == "choice") {
                created = AddNode("choice", ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
                if (Parameter* p = FindParameter(*created, "text")) p->stringValue = ExtractArg(trimmed, "text");
                if (Parameter* p = FindParameter(*created, "target")) p->stringValue = ExtractArg(trimmed, "target");
            } else if (name == "jump") {
                created = AddNode("jump", ImVec2(static_cast<float>(visualIndex) * 280.0f, 0));
                if (Parameter* p = FindParameter(*created, "target")) p->stringValue = ExtractArg(trimmed, "target");
            } else if (name == "var") {
                created = AddNode("variable", ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
                if (Parameter* p = FindParameter(*created, "var")) p->stringValue = ExtractArg(trimmed, "var");
                if (Parameter* p = FindParameter(*created, "op")) p->stringValue = ExtractArg(trimmed, "op");
                if (Parameter* p = FindParameter(*created, "value")) p->stringValue = ExtractArg(trimmed, "val");
            } else if (name == "lua") {
                created = AddNode("lua", ImVec2(static_cast<float>(visualIndex) * 280.0f, 0));
                if (Parameter* p = FindParameter(*created, "fn")) p->stringValue = ExtractArg(trimmed, "fn");
                if (Parameter* p = FindParameter(*created, "args")) p->stringValue = "";
            } else if (name == "stopbgm") {
                created = AddNode("stop_bgm", ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
            } else if (FindCustomCommand(name)) {
                created = AddNode(name, ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
                if (created) {
                    for (Parameter& p : created->parameters) {
                        p.stringValue = ExtractArg(trimmed, p.key);
                    }
                }
            } else {
                created = AddNode("raw", ImVec2(static_cast<float>(visualIndex) * 280.0f, 170));
                if (Parameter* p = FindParameter(*created, "line")) p->stringValue = trimmed;
            }
        } else if (!pendingTextSpeaker.empty() || !trimmed.starts_with("[")) {
            appendDialogueText(trimmed);
            continue;
        }

        if (created) {
            if (hasPendingNodePos) {
                created->position = pendingNodePos;
                created->positionInitialized = true;
                hasPendingNodePos = false;
            }
            activeDialogueId = 0;
            activeTextSpeaker.clear();
            activeTextSignature.clear();
            ++visualIndex;
        }
    }
    if (!start || m_nodes.size() < 2) {
        return false;
    }
    CreateDefaultLinkChain();
    m_dirty = true;
    return true;
}

NodeGraphEditor::Node* NodeGraphEditor::AddNode(const std::string& type, ImVec2 position) {
    const NodeTemplate* definition = FindTemplate(type);
    if (!definition) {
        return nullptr;
    }
    Node node;
    node.id = m_nextId++;
    node.type = type;
    node.position = position;
    node.positionInitialized = true;
    node.parameters = definition->defaults;
    for (const PinTemplate& pinTemplate : definition->inputs) {
        node.inputs.push_back(Pin{ m_nextId++, node.id, pinTemplate.label, pinTemplate.type, true, pinTemplate.parameterKey });
    }
    for (const PinTemplate& pinTemplate : definition->outputs) {
        node.outputs.push_back(Pin{ m_nextId++, node.id, pinTemplate.label, pinTemplate.type, false, pinTemplate.parameterKey });
    }
    m_nodes.push_back(std::move(node));
    MarkDirty();
    return &m_nodes.back();
}

void NodeGraphEditor::RemoveNode(int id) {
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const Link& link) {
        const Pin* start = FindPin(link.startPinId);
        const Pin* end = FindPin(link.endPinId);
        return (start && start->nodeId == id) || (end && end->nodeId == id);
    }), m_links.end());
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) { return node.id == id; }), m_nodes.end());
    MarkDirty();
}

void NodeGraphEditor::AddLink(int startPinId, int endPinId) {
    const Pin* start = FindPin(startPinId);
    const Pin* end = FindPin(endPinId);
    if (!CanLink(start, end)) {
        return;
    }
    if (end && end->input) {
        m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.endPinId == endPinId; }), m_links.end());
    }
    m_links.push_back(Link{ m_nextId++, startPinId, endPinId });
    MarkDirty();
}

void NodeGraphEditor::RemoveLink(int id) {
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.id == id; }), m_links.end());
    MarkDirty();
}

void NodeGraphEditor::CreateDefaultLinkChain() {
    for (size_t i = 0; i + 1 < m_nodes.size(); ++i) {
        if (m_nodes[i].outputs.empty() || m_nodes[i + 1].inputs.empty()) {
            continue;
        }
        AddLink(m_nodes[i].outputs.front().id, m_nodes[i + 1].inputs.front().id);
    }
}

void NodeGraphEditor::UpdateNodePositions() {
    if (!m_context) {
        return;
    }
    ed::SetCurrentEditor(m_context);
    for (Node& node : m_nodes) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        if (std::abs(position.x - node.position.x) > 0.25f || std::abs(position.y - node.position.y) > 0.25f) {
            node.position = position;
            MarkDirty();
        }
    }
}

void NodeGraphEditor::MarkDirty() {
    m_dirty = true;
}

void NodeGraphEditor::Render() {
    EnsureContext();
    if (!m_loaded) {
        LoadOrCreate();
    }
    RenderToolbar();
    ImGui::Spacing();
    RenderGraph();
}

void NodeGraphEditor::RenderToolbar() {
    ImGui::BeginChild((Title() + "Toolbar").c_str(), ImVec2(0, m_kind == GraphKind::PDSDialogue ? 74.0f : 42.0f), false);
    if (m_kind == GraphKind::PDSDialogue) {
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##pds-document", "Script/chapter1.pds", &m_documentPathInput);
        ImGui::SameLine();
        if (ImGui::Button("Open PDS")) OpenDocument(m_documentPathInput);
        ImGui::SameLine();
        if (ImGui::Button("New PDS")) NewDocument(m_documentPathInput);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", CurrentRuntimePath().c_str(), m_dirty ? " *" : "");
    }
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) Reload();
    ImGui::SameLine();
    if (ImGui::Button("Frame")) FrameSelection();
    ImGui::SameLine();
    if (ImGui::Button("Compile")) Log("Compiled " + CurrentRuntimePath() + " (" + std::to_string(Compile().size()) + " bytes)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##node-search", "Search nodes...", &m_search);
    ImGui::SameLine();
    if (m_kind != GraphKind::PDSDialogue) {
        ImGui::TextDisabled("%s%s", DocumentPath().filename().string().c_str(), m_dirty ? " *" : "");
    }
    ImGui::EndChild();
}

void NodeGraphEditor::RenderGraph() {
    ed::SetCurrentEditor(m_context);
    ed::Begin(Title().c_str(), ImVec2(0, 0));

    for (Node& node : m_nodes) {
        RenderNode(node);
    }
    for (const Link& link : m_links) {
        const Pin* start = FindPin(link.startPinId);
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPinId), ed::PinId(link.endPinId), start ? PinColor(start->type) : ImColor(255, 255, 255), 2.0f);
    }

    HandleInteractions();
    RefreshSelection();
    UpdateNodePositions();
    if (m_navigateCountdown > 0 && --m_navigateCountdown == 0) {
        ed::NavigateToContent();
    }

    ed::Suspend();
    ed::NodeId contextNodeId = 0;
    ed::LinkId contextLinkId = 0;
    if (ed::ShowNodeContextMenu(&contextNodeId)) {
        m_contextNodeId = static_cast<int>(contextNodeId.Get());
        ImGui::OpenPopup("Node Menu");
    } else if (ed::ShowLinkContextMenu(&contextLinkId)) {
        m_contextLinkId = static_cast<int>(contextLinkId.Get());
        ImGui::OpenPopup("Link Menu");
    } else if (ed::ShowBackgroundContextMenu()) {
        m_pendingPinId = 0;
        m_createPosition = ImGui::GetMousePos();
        m_createPopup = true;
        ImGui::OpenPopup("Create Node");
    }
    RenderCreatePopup();
    RenderNodeContextMenu();
    ed::Resume();

    ed::End();

    const ImRect graphRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::BeginDragDropTargetCustom(graphRect, ImGui::GetID((Title() + "Drop").c_str()))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
            std::string asset(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
            ApplyAssetToSelection(asset);
        }
        ImGui::EndDragDropTarget();
    }
}

void NodeGraphEditor::RenderNode(Node& node) {
    const NodeTemplate* definition = FindTemplate(node.type);
    if (!definition) {
        return;
    }
    if (node.positionInitialized) {
        ed::SetNodePosition(ed::NodeId(node.id), node.position);
        node.positionInitialized = false;
    }

    const bool selected = ed::IsNodeSelected(ed::NodeId(node.id));
    const ImColor nodeFill = selected ? ImColor(28, 37, 54, 250) : ImColor(19, 24, 38, 240);
    const ImColor nodeBorder = selected ? definition->accent : ImColor(70, 86, 111, 220);
    ImRect headerRect;

    ed::PushStyleColor(ed::StyleColor_NodeBg, nodeFill);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorder);
    ed::BeginNode(ed::NodeId(node.id));
    ImGui::PushID(node.id);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));

    ImGui::BeginGroup();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kNodeContentWidth - 24.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.98f, 1.0f, 1.0f), "%s", definition->title.c_str());
    ImGui::TextColored(ImVec4(0.62f, 0.70f, 0.80f, 1.0f), "%s", definition->description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    headerRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (ImGui::BeginTable("node-layout", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoKeepColumnsVisible, ImVec2(kNodeContentWidth, 0.0f))) {
        ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthFixed, kInputColumnWidth);
        ImGui::TableSetupColumn("outputs", ImGuiTableColumnFlags_WidthFixed, kOutputColumnWidth);
        const size_t rows = std::max(node.inputs.size(), node.outputs.size());
        for (size_t row = 0; row < rows; ++row) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (row < node.inputs.size()) {
                RenderPin(node, node.inputs[row]);
            } else {
                ImGui::Dummy(ImVec2(0.0f, 22.0f));
            }
            ImGui::TableSetColumnIndex(1);
            if (row < node.outputs.size()) {
                RenderPin(node, node.outputs[row]);
            } else {
                ImGui::Dummy(ImVec2(0.0f, 22.0f));
            }
        }
        ImGui::EndTable();
    }

    if (node.type == "dialogue") {
        RenderDialogueTextEditor(node);
    } else {
        RenderUnboundParameters(node);
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);

    ImDrawList* draw = ed::GetNodeBackgroundDrawList(ed::NodeId(node.id));
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    draw->AddRectFilled(rect.Min, rect.Max, nodeFill, kNodeRounding, ImDrawFlags_RoundCornersAll);

    const float halfBorder = ed::GetStyle().NodeBorderWidth * 0.5f;
    const ImVec2 headerMin(rect.Min.x + halfBorder, rect.Min.y + halfBorder);
    const ImVec2 headerMax(rect.Max.x - halfBorder, std::min(rect.Max.y, headerRect.Max.y + 10.0f));
    if (headerMax.x > headerMin.x && headerMax.y > headerMin.y) {
        if (m_headerTexture && m_headerTextureWidth > 0 && m_headerTextureHeight > 0) {
            const ImVec2 uv(
                std::clamp((headerMax.x - headerMin.x) / (kHeaderTextureScale * static_cast<float>(m_headerTextureWidth)), 0.0f, 1.0f),
                std::clamp((headerMax.y - headerMin.y) / (kHeaderTextureScale * static_cast<float>(m_headerTextureHeight)), 0.0f, 1.0f));
            ImColor headerTint = definition->accent;
            headerTint.Value.w = selected ? 0.90f : 0.84f;
            draw->AddImageRounded(m_headerTexture, headerMin, headerMax, ImVec2(0.0f, 0.0f), uv, headerTint, kNodeRounding, ImDrawFlags_RoundCornersTop);
            draw->AddRectFilled(headerMin, headerMax, selected ? IM_COL32(8, 12, 20, 56) : IM_COL32(8, 12, 20, 84), kNodeRounding, ImDrawFlags_RoundCornersTop);
        } else {
            ImColor fallback = definition->accent;
            fallback.Value.w = selected ? 0.50f : 0.38f;
            draw->AddRectFilled(headerMin, headerMax, fallback, kNodeRounding, ImDrawFlags_RoundCornersTop);
        }
    }

    draw->AddRect(rect.Min, rect.Max, nodeBorder, kNodeRounding, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.2f);
}

void NodeGraphEditor::RenderPin(Node& node, const Pin& pin) {
    const bool linked = IsPinLinked(pin.id);
    const ImColor color = PinColor(pin.type);
    const IconType icon = pin.type == PinType::Flow ? IconType::Flow : IconType::Circle;
    ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
    ed::PushStyleVar(ed::StyleVar_PivotAlignment, pin.input ? ImVec2(0, 0.5f) : ImVec2(1, 0.5f));
    ed::BeginPin(ed::PinId(pin.id), pin.input ? ed::PinKind::Input : ed::PinKind::Output);
    if (pin.input) {
        ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), icon, linked, color, ImColor(25, 30, 41));
        if (!pin.label.empty()) {
            ImGui::SameLine(0.0f, kPinLabelSpacing);
            ImGui::TextUnformatted(pin.label.c_str());
        }
    } else {
        const float labelWidth = pin.label.empty() ? 0.0f : ImGui::CalcTextSize(pin.label.c_str()).x;
        const float totalWidth = labelWidth + (pin.label.empty() ? kPinSize : kPinSize + kPinLabelSpacing);
        const float offset = std::max(0.0f, kOutputAlignWidth - totalWidth);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        if (!pin.label.empty()) {
            ImGui::TextUnformatted(pin.label.c_str());
            ImGui::SameLine(0.0f, kPinLabelSpacing);
        }
        ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), icon, linked, color, ImColor(25, 30, 41));
    }
    ed::EndPin();
    ed::PopStyleVar(2);

    if (node.type == "dialogue" && pin.parameterKey == "text") {
        return;
    }

    if (pin.input && !pin.parameterKey.empty() && !linked) {
        if (Parameter* parameter = FindParameter(node, pin.parameterKey)) {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::Indent(24.0f);
            RenderParameter(*parameter, true);
            ImGui::Unindent(24.0f);
        }
    }
}

void NodeGraphEditor::RenderDialogueTextEditor(Node& node) {
    Parameter* speaker = FindParameter(node, "speaker");
    Parameter* character = FindParameter(node, "character");
    Parameter* color = FindParameter(node, "color");
    Parameter* outline = FindParameter(node, "outline");
    Parameter* speed = FindParameter(node, "speed");
    Parameter* effect = FindParameter(node, "effect");
    Parameter* text = FindParameter(node, "text");
    if (!speaker || !character || !color || !outline || !speed || !effect || !text) {
        RenderUnboundParameters(node);
        return;
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::BeginGroup();
    ImGui::TextDisabled("Speaker");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(148.0f);
    bool changed = ImGui::InputText("##speaker", &speaker->stringValue);
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("Char");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(112.0f);
    changed |= ImGui::InputText("##character", &character->stringValue);
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("Speed");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(74.0f);
    changed |= ImGui::DragInt("##speed", &speed->intValue, 1.0f, static_cast<int>(speed->minValue), static_cast<int>(speed->maxValue));

    ImGui::TextDisabled("Color");
    ImGui::SameLine(0.0f, 8.0f);
    changed |= ImGui::ColorEdit4("##text-color", &color->colorValue.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::TextDisabled("Outline");
    ImGui::SameLine(0.0f, 8.0f);
    changed |= ImGui::ColorEdit4("##outline-color", &outline->colorValue.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::TextDisabled("Effect");
    ImGui::SameLine(0.0f, 8.0f);
    const std::string effectLabel = effect->stringValue.empty() ? "none" : effect->stringValue;
    ImGui::SetNextItemWidth(112.0f);
    if (ImGui::BeginCombo("##effect", effectLabel.c_str())) {
        for (const std::string& option : effect->options) {
            const bool selected = option == effectLabel;
            if (ImGui::Selectable(option.c_str(), selected)) {
                effect->stringValue = option;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    std::vector<std::string> lines = SplitTextLines(text->stringValue);
    int removeIndex = -1;
    int insertAfter = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%02d", static_cast<int>(i + 1));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::SetNextItemWidth(392.0f);
        if (ImGui::InputText("##line", &lines[i])) {
            changed = true;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("+")) {
            insertAfter = static_cast<int>(i);
        }
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::BeginDisabled(lines.size() <= 1);
        if (ImGui::SmallButton("-")) {
            removeIndex = static_cast<int>(i);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Add Line")) {
        insertAfter = static_cast<int>(lines.empty() ? 0 : lines.size() - 1);
    }

    if (insertAfter >= 0) {
        lines.insert(lines.begin() + insertAfter + 1, std::string{});
        changed = true;
    }
    if (removeIndex >= 0 && lines.size() > 1) {
        lines.erase(lines.begin() + removeIndex);
        changed = true;
    }
    if (changed) {
        text->stringValue = JoinTextLines(lines);
        MarkDirty();
    }
    ImGui::EndGroup();
}

void NodeGraphEditor::RenderUnboundParameters(Node& node) {
    std::vector<Parameter*> fields;
    for (Parameter& parameter : node.parameters) {
        const auto pinIt = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const Pin& pin) {
            return pin.parameterKey == parameter.key;
        });
        if (pinIt == node.inputs.end()) {
            fields.push_back(&parameter);
        }
    }
    if (fields.empty()) {
        return;
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    for (Parameter* parameter : fields) {
        ImGui::TextDisabled("%s", parameter->label.c_str());
        RenderParameter(*parameter, true);
    }
}

void NodeGraphEditor::HandleInteractions() {
    if (ed::BeginCreate(ImColor(137, 208, 255), 2.5f)) {
        ed::PinId start = 0;
        ed::PinId end = 0;
        if (ed::QueryNewLink(&start, &end)) {
            const Pin* a = FindPin(static_cast<int>(start.Get()));
            const Pin* b = FindPin(static_cast<int>(end.Get()));
            if (a && b && a->input) {
                std::swap(a, b);
                std::swap(start, end);
            }
            if (CanLink(a, b)) {
                ImGui::SetTooltip("Create link");
                if (ed::AcceptNewItem(ImColor(136, 255, 178), 3.0f)) {
                    AddLink(static_cast<int>(start.Get()), static_cast<int>(end.Get()));
                }
            } else {
                ImGui::SetTooltip("Pins are not compatible");
                ed::RejectNewItem(ImColor(255, 96, 96), 2.0f);
            }
        }

        ed::PinId newNodePin = 0;
        if (ed::QueryNewNode(&newNodePin) && ed::AcceptNewItem()) {
            m_pendingPinId = static_cast<int>(newNodePin.Get());
            m_createPosition = ImGui::GetMousePos();
            ImGui::OpenPopup("Create Node");
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::NodeId nodeId = 0;
        while (ed::QueryDeletedNode(&nodeId)) {
            if (ed::AcceptDeletedItem()) {
                RemoveNode(static_cast<int>(nodeId.Get()));
            }
        }
        ed::LinkId linkId = 0;
        while (ed::QueryDeletedLink(&linkId)) {
            if (ed::AcceptDeletedItem()) {
                RemoveLink(static_cast<int>(linkId.Get()));
            }
        }
    }
    ed::EndDelete();
}

void NodeGraphEditor::RefreshSelection() {
    std::array<ed::NodeId, 8> nodes{};
    const int nodeCount = ed::GetSelectedNodes(nodes.data(), static_cast<int>(nodes.size()));
    m_selectedNodeId = nodeCount > 0 ? static_cast<int>(nodes.front().Get()) : 0;

    std::array<ed::LinkId, 8> links{};
    const int linkCount = ed::GetSelectedLinks(links.data(), static_cast<int>(links.size()));
    m_selectedLinkId = linkCount > 0 ? static_cast<int>(links.front().Get()) : 0;
}

void NodeGraphEditor::RenderCreatePopup() {
    if (!ImGui::BeginPopup("Create Node")) {
        return;
    }
    const ImVec2 canvasPosition = ed::ScreenToCanvas(m_createPosition);
    std::string currentCategory;
    const std::string lowerSearch = Lower(m_search);
    for (const NodeTemplate& definition : m_library) {
        const std::string searchable = Lower(definition.title + " " + definition.category + " " + definition.description);
        if (!lowerSearch.empty() && searchable.find(lowerSearch) == std::string::npos) {
            continue;
        }
        if (definition.category != currentCategory) {
            currentCategory = definition.category;
            ImGui::SeparatorText(currentCategory.c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Text, definition.accent.Value);
        const bool clicked = ImGui::MenuItem(definition.title.c_str(), nullptr, false);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", definition.description.c_str());
        }
        if (clicked) {
            Node* node = AddNode(definition.type, canvasPosition);
            if (node && m_pendingPinId) {
                Pin* pending = FindPin(m_pendingPinId);
                if (pending) {
                    if (pending->input && !node->outputs.empty()) AddLink(node->outputs.front().id, pending->id);
                    if (!pending->input && !node->inputs.empty()) AddLink(pending->id, node->inputs.front().id);
                }
            }
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void NodeGraphEditor::RenderNodeContextMenu() {
    if (ImGui::BeginPopup("Node Menu")) {
        if (const Node* node = FindNode(m_contextNodeId)) {
            if (const NodeTemplate* definition = FindTemplate(node->type)) {
                ImGui::TextUnformatted(definition->title.c_str());
                ImGui::TextDisabled("%s", definition->description.c_str());
                ImGui::Separator();
            }
        }
        if (ImGui::MenuItem("Delete")) {
            RemoveNode(m_contextNodeId);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Link Menu")) {
        if (ImGui::MenuItem("Delete")) {
            RemoveLink(m_contextLinkId);
        }
        ImGui::EndPopup();
    }
}

void NodeGraphEditor::RenderInspector() {
    EnsureContext();
    if (!m_loaded) {
        LoadOrCreate();
    }
    if (m_context) {
        ed::SetCurrentEditor(m_context);
        RefreshSelection();
    }

    ImGui::SeparatorText("Document");
    ImGui::TextWrapped("%s", CurrentRuntimePath().c_str());
    ImGui::TextDisabled("%d nodes, %d links%s", static_cast<int>(m_nodes.size()), static_cast<int>(m_links.size()), m_dirty ? "  (dirty)" : "");

    if (Node* node = FindNode(m_selectedNodeId)) {
        const NodeTemplate* definition = FindTemplate(node->type);
        ImGui::SeparatorText("Node");
        ImGui::TextUnformatted(definition ? definition->title.c_str() : node->type.c_str());
        if (definition) ImGui::TextWrapped("%s", definition->description.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("id %d  type %s", node->id, node->type.c_str());
        float position[2]{ node->position.x, node->position.y };
        if (ImGui::DragFloat2("Position", position, 1.0f)) {
            node->position = ImVec2(position[0], position[1]);
            ed::SetNodePosition(ed::NodeId(node->id), node->position);
            MarkDirty();
        }
        if (node->type == "dialogue") {
            ImGui::SeparatorText("Dialogue Text");
            ImGui::TextWrapped("Edit speaker, text args, and dialogue lines directly inside the Dialogue Text node.");
        } else {
            ImGui::SeparatorText("Fields");
            if (node->parameters.empty()) {
                ImGui::TextDisabled("This node has no configurable parameters yet.");
            }
            for (Parameter& parameter : node->parameters) {
                RenderParameter(parameter, false);
            }
        }
        RenderPinSummary(*node);
        return;
    }
    if (const Link* link = FindLink(m_selectedLinkId)) {
        ImGui::SeparatorText("Link");
        ImGui::Text("Link %d", link->id);
        ImGui::TextDisabled("From pin %d to pin %d", link->startPinId, link->endPinId);
        return;
    }
    RenderGraphOverview();
}

void NodeGraphEditor::RenderGraphOverview() {
    ImGui::SeparatorText("Graph");
    ImGui::TextDisabled("%s", SelectionSummary().c_str());
    ImGui::TextWrapped("Select a node to edit its fields. Right click the graph canvas to create nodes, or drag resources from the Resource Browser.");

    ImGui::SeparatorText("Node Library");
    std::string currentCategory;
    for (const NodeTemplate& definition : m_library) {
        if (definition.category != currentCategory) {
            currentCategory = definition.category;
            ImGui::Spacing();
            ImGui::TextColored(definition.accent.Value, "%s", currentCategory.c_str());
        }
        ImGui::BulletText("%s", definition.title.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", definition.description.c_str());
        }
    }
}

void NodeGraphEditor::RenderPinSummary(const Node& node) const {
    ImGui::SeparatorText("Pins");
    if (node.inputs.empty() && node.outputs.empty()) {
        ImGui::TextDisabled("No pins.");
        return;
    }
    if (!node.inputs.empty()) {
        ImGui::TextDisabled("Inputs");
        for (const Pin& pin : node.inputs) {
            ImGui::BulletText("%s  (%s)", pin.label.c_str(), PinTypeName(pin.type));
        }
    }
    if (!node.outputs.empty()) {
        ImGui::TextDisabled("Outputs");
        for (const Pin& pin : node.outputs) {
            ImGui::BulletText("%s  (%s)", pin.label.c_str(), PinTypeName(pin.type));
        }
    }
}

void NodeGraphEditor::RenderParameter(Parameter& parameter, bool compact) {
    ImGui::PushID(parameter.key.c_str());
    const std::string controlId = "##value";
    if (!compact) {
        ImGui::TextUnformatted(parameter.label.c_str());
    }
    if (compact) {
        ImGui::SetNextItemWidth(kCompactParameterWidth);
    } else {
        ImGui::SetNextItemWidth(-1.0f);
    }
    bool changed = false;
    switch (parameter.type) {
        case ParamType::Bool:
            changed = ImGui::Checkbox(controlId.c_str(), &parameter.boolValue);
            break;
        case ParamType::Int:
            changed = ImGui::DragInt(controlId.c_str(), &parameter.intValue, 1.0f, static_cast<int>(parameter.minValue), static_cast<int>(parameter.maxValue));
            break;
        case ParamType::Float:
            changed = ImGui::DragFloat(controlId.c_str(), &parameter.floatValue, 0.1f, parameter.minValue, parameter.maxValue);
            break;
        case ParamType::Color:
            changed = ImGui::ColorEdit4(controlId.c_str(), &parameter.colorValue.x);
            break;
        case ParamType::Option:
            if (ImGui::BeginCombo(controlId.c_str(), parameter.stringValue.c_str())) {
                for (const std::string& option : parameter.options) {
                    const bool selected = option == parameter.stringValue;
                    if (ImGui::Selectable(option.c_str(), selected)) {
                        parameter.stringValue = option;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        case ParamType::String:
        case ParamType::Asset:
            if (parameter.multiline && parameter.type == ParamType::String) {
                changed = ImGui::InputTextMultiline(controlId.c_str(), &parameter.stringValue, ImVec2(compact ? kCompactParameterWidth : -1.0f, compact ? 96.0f : 156.0f));
            } else {
                changed = ImGui::InputText(controlId.c_str(), &parameter.stringValue, compact && parameter.type == ParamType::Asset ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
            }
            if (parameter.type == ParamType::Asset) {
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
                        parameter.stringValue.assign(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
                        changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }
                const std::string selected = m_selectedResource ? m_selectedResource() : std::string{};
                if (compact) {
                    if (!selected.empty()) {
                        ImGui::SameLine(0.0f, 4.0f);
                        if (ImGui::SmallButton("Use")) {
                            parameter.stringValue = selected;
                            changed = true;
                        }
                    }
                    ImGui::SameLine(0.0f, 4.0f);
                    if (ImGui::SmallButton("X")) {
                        parameter.stringValue.clear();
                        changed = true;
                    }
                } else if (!selected.empty() && ImGui::SmallButton("Use Selected Asset")) {
                    parameter.stringValue = selected;
                    changed = true;
                }
            }
            break;
    }
    if (changed) MarkDirty();
    ImGui::PopID();
}

void NodeGraphEditor::ApplyAssetToSelection(const std::string& runtimePath) {
    if (runtimePath.empty()) {
        return;
    }
    if (m_kind == GraphKind::PDSDialogue && Lower(fs::path(runtimePath).extension().string()) == ".pds") {
        OpenDocument(runtimePath);
        return;
    }
    if (Node* node = FindNode(m_selectedNodeId)) {
        for (Parameter& parameter : node->parameters) {
            if (parameter.type == ParamType::Asset || parameter.type == ParamType::String) {
                parameter.stringValue = runtimePath;
                MarkDirty();
                Log("Applied asset to node: " + runtimePath);
                return;
            }
        }
    }
    Node* asset = AddNode("asset", ed::ScreenToCanvas(m_createPosition));
    if (asset) {
        if (Parameter* p = FindParameter(*asset, "path")) {
            p->stringValue = runtimePath;
        }
        Log("Created asset node: " + runtimePath);
    }
}

void NodeGraphEditor::FrameSelection() {
    EnsureContext();
    ed::SetCurrentEditor(m_context);
    ed::NavigateToContent();
}

std::vector<ExportArtifact> NodeGraphEditor::BuildArtifacts() const {
    return { ExportArtifact{ Title(), fs::path(CurrentRuntimePath()), Compile() } };
}

std::string NodeGraphEditor::Compile() const {
    return m_kind == GraphKind::PDSDialogue ? CompilePDS() : CompileGameloop();
}

const NodeGraphEditor::NodeTemplate* NodeGraphEditor::FindTemplate(std::string_view type) const {
    auto it = std::find_if(m_library.begin(), m_library.end(), [&](const NodeTemplate& definition) { return definition.type == type; });
    return it == m_library.end() ? nullptr : &(*it);
}

NodeGraphEditor::Node* NodeGraphEditor::FindNode(int id) {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

const NodeGraphEditor::Node* NodeGraphEditor::FindNode(int id) const {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

NodeGraphEditor::Pin* NodeGraphEditor::FindPin(int id) {
    for (Node& node : m_nodes) {
        for (Pin& pin : node.inputs) if (pin.id == id) return &pin;
        for (Pin& pin : node.outputs) if (pin.id == id) return &pin;
    }
    return nullptr;
}

const NodeGraphEditor::Pin* NodeGraphEditor::FindPin(int id) const {
    for (const Node& node : m_nodes) {
        for (const Pin& pin : node.inputs) if (pin.id == id) return &pin;
        for (const Pin& pin : node.outputs) if (pin.id == id) return &pin;
    }
    return nullptr;
}

NodeGraphEditor::Link* NodeGraphEditor::FindLink(int id) {
    auto it = std::find_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.id == id; });
    return it == m_links.end() ? nullptr : &(*it);
}

const NodeGraphEditor::Link* NodeGraphEditor::FindLink(int id) const {
    auto it = std::find_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.id == id; });
    return it == m_links.end() ? nullptr : &(*it);
}

NodeGraphEditor::Parameter* NodeGraphEditor::FindParameter(Node& node, std::string_view key) {
    auto it = std::find_if(node.parameters.begin(), node.parameters.end(), [&](const Parameter& parameter) { return parameter.key == key; });
    return it == node.parameters.end() ? nullptr : &(*it);
}

const NodeGraphEditor::Parameter* NodeGraphEditor::FindParameter(const Node& node, std::string_view key) const {
    auto it = std::find_if(node.parameters.begin(), node.parameters.end(), [&](const Parameter& parameter) { return parameter.key == key; });
    return it == node.parameters.end() ? nullptr : &(*it);
}

bool NodeGraphEditor::CanLink(const Pin* start, const Pin* end) const {
    if (!start || !end || start == end || start->nodeId == end->nodeId) return false;
    if (start->input || !end->input) return false;
    if (start->type == end->type) return true;
    if (start->type == PinType::Object && end->type != PinType::Flow) return true;
    if (end->type == PinType::Object && start->type != PinType::Flow) return true;
    if (start->type == PinType::Int && end->type == PinType::Float) return true;
    if (start->type == PinType::Float && end->type == PinType::Int) return true;
    if (start->type == PinType::Asset && end->type == PinType::String) return true;
    if (start->type == PinType::String && end->type == PinType::Asset) return true;
    return false;
}

bool NodeGraphEditor::IsPinLinked(int id) const {
    return std::any_of(m_links.begin(), m_links.end(), [&](const Link& link) {
        return link.startPinId == id || link.endPinId == id;
    });
}

const NodeGraphEditor::Node* NodeGraphEditor::FindFlowStart() const {
    auto hasIncoming = [&](const Node& node) {
        for (const Pin& pin : node.inputs) {
            if (pin.type == PinType::Flow && IsPinLinked(pin.id)) return true;
        }
        return false;
    };
    for (const Node& node : m_nodes) {
        if (!hasIncoming(node) && std::any_of(node.outputs.begin(), node.outputs.end(), [](const Pin& pin) { return pin.type == PinType::Flow; })) {
            return &node;
        }
    }
    return m_nodes.empty() ? nullptr : &m_nodes.front();
}

const NodeGraphEditor::Node* NodeGraphEditor::FindFlowNext(const Node& node, int outputIndex) const {
    int seen = 0;
    for (const Pin& pin : node.outputs) {
        if (pin.type != PinType::Flow) continue;
        if (seen++ != outputIndex) continue;
        auto it = std::find_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.startPinId == pin.id; });
        if (it == m_links.end()) return nullptr;
        const Pin* target = FindPin(it->endPinId);
        return target ? FindNode(target->nodeId) : nullptr;
    }
    return nullptr;
}

std::vector<const NodeGraphEditor::Node*> NodeGraphEditor::LinearFlow() const {
    std::vector<const Node*> flow;
    std::unordered_set<int> visited;
    const Node* current = FindFlowStart();
    while (current && visited.insert(current->id).second) {
        flow.push_back(current);
        current = FindFlowNext(*current);
    }
    return flow;
}

std::string NodeGraphEditor::ResolveString(const Node& node, std::string_view key) const {
    auto pinIt = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const Pin& pin) { return pin.parameterKey == key; });
    if (pinIt != node.inputs.end()) {
        auto linkIt = std::find_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.endPinId == pinIt->id; });
        if (linkIt != m_links.end()) {
            const Pin* sourcePin = FindPin(linkIt->startPinId);
            const Node* sourceNode = sourcePin ? FindNode(sourcePin->nodeId) : nullptr;
            if (sourceNode) {
                if (sourceNode->type == "string") {
                    if (const Parameter* p = FindParameter(*sourceNode, "value")) return p->stringValue;
                }
                if (sourceNode->type == "asset") {
                    if (const Parameter* p = FindParameter(*sourceNode, "path")) return p->stringValue;
                }
            }
        }
    }
    if (const Parameter* p = FindParameter(node, key)) return p->stringValue;
    return {};
}

int NodeGraphEditor::ResolveInt(const Node& node, std::string_view key) const {
    if (const Parameter* p = FindParameter(node, key)) {
        return p->type == ParamType::Float ? static_cast<int>(std::round(p->floatValue)) : p->intValue;
    }
    return 0;
}

float NodeGraphEditor::ResolveFloat(const Node& node, std::string_view key) const {
    if (const Parameter* p = FindParameter(node, key)) {
        return p->type == ParamType::Int ? static_cast<float>(p->intValue) : p->floatValue;
    }
    return 0.0f;
}

bool NodeGraphEditor::ResolveBool(const Node& node, std::string_view key) const {
    if (const Parameter* p = FindParameter(node, key)) return p->boolValue;
    return false;
}

ImColor NodeGraphEditor::PinColor(PinType type) const {
    switch (type) {
        case PinType::Flow: return ImColor(245, 248, 255);
        case PinType::Bool: return ImColor(255, 104, 104);
        case PinType::Int: return ImColor(92, 220, 170);
        case PinType::Float: return ImColor(126, 232, 106);
        case PinType::String: return ImColor(175, 118, 255);
        case PinType::Asset: return ImColor(80, 194, 255);
        case PinType::Object: return ImColor(228, 188, 104);
    }
    return ImColor(255, 255, 255);
}

const char* NodeGraphEditor::PinTypeName(PinType type) {
    switch (type) {
        case PinType::Flow: return "Flow";
        case PinType::Bool: return "Bool";
        case PinType::Int: return "Int";
        case PinType::Float: return "Float";
        case PinType::String: return "String";
        case PinType::Asset: return "Asset";
        case PinType::Object: return "Object";
    }
    return "Unknown";
}

std::string NodeGraphEditor::CompilePDS() const {
    std::ostringstream script;
    for (const Node* node : LinearFlow()) {
        const std::string& type = node->type;
        if (type != "pds_start") {
            script << "//@node x=\"" << static_cast<int>(node->position.x) << "\" y=\""
                   << static_cast<int>(node->position.y) << "\"\n";
        }
        if (type == "pds_start") {
            const std::string title = ResolveString(*node, "title");
            if (!title.empty()) script << "# " << title << "\n\n";
        } else if (type == "chapter") {
            script << "# " << ResolveString(*node, "title") << "\n\n";
        } else if (type == "background") {
            script << "[bg file=" << QuoteLua(ResolveString(*node, "file")) << "]\n\n";
        } else if (type == "bgm") {
            script << "[bgm file=" << QuoteLua(ResolveString(*node, "file")) << "]\n\n";
        } else if (type == "character") {
            script << "[char id=" << QuoteLua(ResolveString(*node, "id")) << " expression=" << QuoteLua(ResolveString(*node, "expression")) << " pos=" << QuoteLua(ResolveString(*node, "pos")) << "]\n\n";
        } else if (type == "dialogue") {
            const std::string speaker = ResolveString(*node, "speaker");
            const std::string character = ResolveString(*node, "character");
            const int speed = ResolveInt(*node, "speed");
            const std::string effect = ResolveString(*node, "effect");
            const ImVec4 textColor = FindParameter(*node, "color") ? FindParameter(*node, "color")->colorValue : ImVec4(1, 1, 1, 1);
            const ImVec4 outlineColor = FindParameter(*node, "outline") ? FindParameter(*node, "outline")->colorValue : ImVec4(0, 0, 0, 1);
            script << "[text";
            if (!speaker.empty()) script << " speaker=" << QuoteLua(speaker);
            if (!character.empty()) script << " char=" << QuoteLua(character);
            if (!SameColor(textColor, ImVec4(1, 1, 1, 1))) script << " color=" << QuoteLua(ColorToPDSCsv(textColor));
            if (!SameColor(outlineColor, ImVec4(0, 0, 0, 1))) script << " outline=" << QuoteLua(ColorToPDSCsv(outlineColor));
            if (speed != 40) script << " speed=" << QuoteLua(std::to_string(speed));
            if (!effect.empty() && effect != "none") script << " effect=" << QuoteLua(effect);
            script << "]\n" << ResolveString(*node, "text") << "\n\n";
        } else if (type == "choice") {
            script << "[choice text=" << QuoteLua(ResolveString(*node, "text")) << " target=" << QuoteLua(ResolveString(*node, "target")) << "]\n\n";
        } else if (type == "label") {
            script << "*" << ResolveString(*node, "name") << "\n\n";
        } else if (type == "jump") {
            script << "[jump target=" << QuoteLua(ResolveString(*node, "target")) << "]\n\n";
        } else if (type == "variable") {
            script << "[var var=" << QuoteLua(ResolveString(*node, "var")) << " op=" << QuoteLua(ResolveString(*node, "op")) << " val=" << QuoteLua(ResolveString(*node, "value")) << "]\n\n";
        } else if (type == "lua") {
            script << "[lua fn=" << QuoteLua(ResolveString(*node, "fn"));
            const std::string args = ResolveString(*node, "args");
            if (!args.empty()) script << " " << args;
            script << "]\n\n";
        } else if (type == "transition") {
            script << "[lua fn=\"PXEditorTransition\" style=" << QuoteLua(ResolveString(*node, "style")) << " speed=" << QuoteLua(std::to_string(ResolveFloat(*node, "speed"))) << "]\n\n";
        } else if (type == "animate_actor") {
            script << "[lua fn=\"PXEditorAnimateActor\" target=" << QuoteLua(ResolveString(*node, "target")) << " duration=" << QuoteLua(std::to_string(ResolveFloat(*node, "duration"))) << " move_x=" << QuoteLua(std::to_string(ResolveFloat(*node, "move_x"))) << "]\n\n";
        } else if (type == "spawn_ui") {
            script << "[lua fn=\"PXEditorSpawnUI\" ui_script=" << QuoteLua(ResolveString(*node, "ui")) << "]\n\n";
        } else if (type == "stop_bgm") {
            script << "[stopbgm]\n\n";
        } else if (type == "raw") {
            script << ResolveString(*node, "line") << "\n\n";
        } else if (FindCustomCommand(type)) {
            script << "[" << type;
            for (const Parameter& p : node->parameters) {
                script << " " << p.key << "=" << QuoteLua(p.stringValue);
            }
            script << "]\n\n";
        }
    }
    return script.str();
}

std::string NodeGraphEditor::CompileGameloop() const {
    auto flow = LinearFlow();
    auto find = [&](std::string_view type) -> const Node* {
        auto it = std::find_if(flow.begin(), flow.end(), [&](const Node* node) { return node->type == type; });
        return it == flow.end() ? nullptr : *it;
    };

    const Node* boot = find("boot");
    const Node* config = find("configure");
    const Node* splash = find("splash");
    const Node* scene = find("switch_scene");

    const std::string font = boot ? ResolveString(*boot, "font") : "NotoSansTC-Bold.ttf";
    const int fontSize = boot ? ResolveInt(*boot, "font_size") : 32;
    const std::string transition = config ? ResolveString(*config, "transition") : "dissolve";
    const int speed = config ? ResolveInt(*config, "speed") : 10;
    const int notification = config ? ResolveInt(*config, "notification") : 20;
    const std::string sceneModule = scene ? ResolveString(*scene, "scene") : "Scripts/scenes/title_scene.lua";
    const std::string sceneCtor = scene ? ResolveString(*scene, "ctor") : "TitleScene";
    const std::string startPds = scene ? ResolveString(*scene, "pds") : "Script/chapter1.pds";

    std::ostringstream lua;
    lua << "-- Generated by PrismatiX Editor\n";
    lua << "local loadedModules = {}\n";
    lua << "local function include(path)\n";
    lua << "    if loadedModules[path] then return loadedModules[path] end\n";
    lua << "    local source = Engine.ReadAssetText(path, true)\n";
    lua << "    if not source then error(\"Failed to load script: \" .. path) end\n";
    lua << "    local chunk, err = load(source, \"@\" .. path)\n";
    lua << "    if not chunk then error(\"Lua parse error in \" .. path .. \": \" .. err) end\n";
    lua << "    local result = chunk()\n";
    lua << "    loadedModules[path] = result or true\n";
    lua << "    return loadedModules[path]\n";
    lua << "end\n\n";
    lua << "_G.include = include\n";
    lua << "_G.Utils   = include(\"Scripts/common/utils.lua\")\n";
    lua << "_G.Runtime = include(\"Scripts/common/runtime_helpers.lua\")\n";
    lua << "_G.UI      = include(\"Scripts/ui/runtime.lua\")\n";
    lua << "_G.Ease    = include(\"Scripts/common/easing.lua\")\n\n";
    lua << "local SceneManager        = include(\"Scripts/common/scene_manager.lua\")\n";
    lua << "local Transition          = include(\"Scripts/fx/transition.lua\")\n";
    lua << "local NotificationManager = include(\"Scripts/components/notification.lua\")\n";
    lua << "local FX                  = include(\"Scripts/fx/screen_fx.lua\")\n\n";
    lua << "function Entrypoint()\n";
    lua << "    local fontName = " << QuoteLua(font) << "\n";
    lua << "    local fontSize = " << fontSize << "\n";
    lua << "    _G.PX = {\n";
    lua << "        Scene        = SceneManager.new(),\n";
    lua << "        Transition   = Transition.new({ style = " << QuoteLua(transition) << ", speed = " << speed << " }),\n";
    lua << "        Notification = NotificationManager.new(fontName, " << notification << "),\n";
    lua << "        FX           = FX,\n";
    lua << "    }\n\n";
    if (splash) {
        lua << "    Runtime.run_splash_script(" << QuoteLua(ResolveString(*splash, "engine")) << ")\n";
        lua << "    Runtime.run_splash_script(" << QuoteLua(ResolveString(*splash, "game")) << ")\n";
    }
    lua << "    local FirstScene = include(" << QuoteLua(sceneModule) << ")\n";
    lua << "    _G.PX.GeneratedSceneScript = " << QuoteLua(startPds) << "\n";
    lua << "    _G.PX.Scene:switch(FirstScene.new(fontName, fontSize, " << QuoteLua(startPds) << "))\n\n";
    lua << "    while Engine.IsRunning() do\n";
    lua << "        Engine.HandleEvents()\n";
    lua << "        local winW, winH = Engine.GetLogicalSize()\n";
    lua << "        local mx, my = Engine.GetMouseX(), Engine.GetMouseY()\n";
    lua << "        local leftClick = Engine.GetLeftClick()\n";
    lua << "        local rightClick = Engine.GetRightClick()\n";
    lua << "        _G.PX.FX.update()\n";
    lua << "        local ox, oy = _G.PX.FX.get_offset()\n";
    lua << "        Engine.SetCameraOffset(ox, oy)\n";
    lua << "        _G.PX.Notification:update()\n";
    lua << "        _G.PX.Transition:update(winW)\n";
    lua << "        _G.PX.Scene:update(mx, my, leftClick, rightClick)\n";
    lua << "        Engine.ClearScreen(0, 0, 0, 255)\n";
    lua << "        _G.PX.Scene:render(winW, winH)\n";
    lua << "        _G.PX.Notification:render(winW, winH)\n";
    lua << "        _G.PX.Transition:draw_fullscreen(winW, winH)\n";
    lua << "        Engine.PresentScreen()\n";
    lua << "    end\n";
    lua << "end\n";
    return lua.str();
}

std::string NodeGraphEditor::QuoteLua(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out.push_back(ch); break;
        }
    }
    out += "\"";
    return out;
}

std::string NodeGraphEditor::Trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) ++start;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return std::string(value.substr(start, end - start));
}

std::string NodeGraphEditor::Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string NodeGraphEditor::NormalizePDSRuntimePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = Trim(value);
    if (value.empty()) {
        return {};
    }
    if (value.rfind("Data/", 0) == 0) {
        value.erase(0, 5);
    }
    if (value.find('/') == std::string::npos) {
        value = "Script/" + value;
    }
    if (!Lower(value).ends_with(".pds")) {
        value += ".pds";
    }
    return value;
}

std::string NodeGraphEditor::SelectionSummary() const {
    if (const Node* node = FindNode(m_selectedNodeId)) {
        if (const NodeTemplate* definition = FindTemplate(node->type)) {
            return definition->title + " selected";
        }
    }
    if (FindLink(m_selectedLinkId)) {
        return "Link selected";
    }
    return "No graph item selected";
}

void NodeGraphEditor::Log(const std::string& message) const {
    if (m_log) {
        m_log(message);
    }
}

}
