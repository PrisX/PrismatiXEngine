#define IMGUI_DEFINE_MATH_OPERATORS

#include "NodeGraphEditor.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <widgets.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>
#include "Engine/IO/AtomicFile.h"
#include "Engine/VN/Expression/Expression.h"

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
    if (!text.empty() && text.back() == '\n') {
        lines.emplace_back();
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

std::string ColorToCsv(const ImVec4& color) {
    std::ostringstream out;
    out << static_cast<int>(std::round(std::clamp(color.x, 0.0f, 1.0f) * 255.0f)) << "," << static_cast<int>(std::round(std::clamp(color.y, 0.0f, 1.0f) * 255.0f)) << "," << static_cast<int>(std::round(std::clamp(color.z, 0.0f, 1.0f) * 255.0f));
    return out.str();
}

Json ColorToJson(const ImVec4& color) {
    return Json::array(
        {
            static_cast<int>(std::round(color.x * 255.0f)),
            static_cast<int>(std::round(color.y * 255.0f)),
            static_cast<int>(std::round(color.z * 255.0f)),
            static_cast<int>(std::round(color.w * 255.0f)),
        }
    );
}

ImVec4 ColorFromJson(const Json& json, ImVec4 fallback) {
    if (!json.is_array() || json.empty()) {
        return fallback;
    }
    return ImVec4(
        json.size() > 0 ? std::clamp(json[0].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.x,
        json.size() > 1 ? std::clamp(json[1].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.y,
        json.size() > 2 ? std::clamp(json[2].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.z,
        json.size() > 3 ? std::clamp(json[3].get<float>() / 255.0f, 0.0f, 1.0f) : fallback.w
    );
}

}  // namespace

NodeGraphEditor::NodeGraphEditor(GraphKind kind, LogSink log) : m_kind(kind), m_log(std::move(log)),m_editDocumentId(Uuid::Random()),m_editHistory(*this) {
    if (m_kind == GraphKind::Scenario) {
        m_documentRuntimePath = "Content/Scenario/start.pxscenario";
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
    m_fieldOptionsCache.clear();
    BuildLibrary();
    // Loaded lazily on the next Render()/OpenDocument(): a freshly created tab
    // must not touch the default document path on disk.
    m_loaded = false;
}

void NodeGraphEditor::SetSelectedResourceCallback(SelectedResourceCallback callback) { m_selectedResource = std::move(callback); }

void NodeGraphEditor::SetHeaderTexture(ImTextureID texture, int width, int height) {
    m_headerTexture = texture;
    m_headerTextureWidth = width;
    m_headerTextureHeight = height;
}

std::string NodeGraphEditor::Title() const { return "Scenario Node Editor"; }

fs::path NodeGraphEditor::DocumentPath() const {
    if (!m_project || !m_project->IsOpen()) {
        return {};
    }
    return m_project->root / fs::path(m_documentRuntimePath.empty() ? "Content/Scenario/start.pxscenario" : m_documentRuntimePath);
}

std::string NodeGraphEditor::CurrentRuntimePath() const { return m_documentRuntimePath.empty() ? "Content/Scenario/start.pxscenario" : m_documentRuntimePath; }

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

    const auto in = [](std::string label, PinType type = PinType::Flow, std::string key = {}) { return PinTemplate{ std::move(label), type, true, std::move(key) }; };
    const auto out = [](std::string label, PinType type = PinType::Flow, std::string key = {}) { return PinTemplate{ std::move(label), type, false, std::move(key) }; };

    if (m_kind == GraphKind::Scenario) {
        m_library = {
            { "scenario_start", "Start", "Timeline", "Entry for this Scenario document.", ImColor(80, 184, 255), {}, { out("Begin") }, { paramString("title", "Scenario Title", "第一章") } },
            { "chapter", "Chapter", "Timeline", "Emits a chapter heading.", ImColor(80, 184, 255), { in("In") }, { out("Out") }, { paramString("title", "Title", "第一章") } },
            { "background", "Background", "Stage", "Sets the current background image.", ImColor(67, 186, 255), { in("In"), in("Image", PinType::Asset, "file") }, { out("Out") }, { paramString("file", "Image", "", ParamType::Asset) } },
            { "bgm", "Play BGM", "Audio", "Starts background music.", ImColor(255, 126, 95), { in("In"), in("BGM", PinType::Asset, "file") }, { out("Out") }, { paramString("file", "Audio", "", ParamType::Asset) } },
            { "character",
              "Character",
              "Stage",
              "Places a character sprite.",
              ImColor(255, 194, 97),
              { in("In") },
              { out("Out") },
              { paramString("id", "Character", ""), paramString("file", "Sprite Override", "", ParamType::Asset), paramString("expression", "Expression", ""), paramOption("pos", "Position", "2", {"1","2","3"}),
                paramFloat("x", "Offset X", 0.0f, -1920.0f, 1920.0f), paramFloat("y", "Offset Y", 0.0f, -1080.0f, 1080.0f),
                paramFloat("scale", "Scale", 1.0f, 0.1f, 4.0f) } },
            { "dialogue",
              "Dialogue Text",
              "Dialogue",
              "Shows one or more speaker lines with the same text format.",
              ImColor(160, 132, 255),
              { in("In"), in("Text", PinType::String, "text") },
              { out("Out") },
              { paramString("speaker", "Speaker Override", ""),
                paramString("character", "Character", ""),
                paramColor("color", "Color", ImVec4(1, 1, 1, 1)),
                paramColor("outline", "Outline", ImVec4(0, 0, 0, 1)),
                paramInt("speed", "Speed", 40, 0, 240),
                paramOption("effect", "Effect", "none", { "none", "shake", "pulse" }),
                paramString("voices", "Voices (per line)", "", ParamType::String, true),
                paramString("text", "Text", "新的對話", ParamType::String, true) } },
            { "choice", "Choice", "Dialogue", "Adds one visible option. Chain Next to another Choice and connect Choice to its result.", ImColor(103, 219, 177), { in("In") }, { out("Next"), out("Choice") }, { paramString("text", "Text", "選項") } },
            { "label", "Label", "Flow", "Defines a jump target.", ImColor(103, 219, 177), { in("In") }, { out("Out") }, { paramString("name", "Label", "end") } },
            { "jump", "Jump", "Flow", "Jumps to a label or another script.", ImColor(103, 219, 177), { in("In"), in("Target", PinType::String, "target") }, { out("Out") }, { paramString("target", "Target", "") } },
            { "variable",
              "Variable",
              "Logic",
              "Sets or changes a VN variable.",
              ImColor(232, 178, 92),
              { in("In") },
              { out("Out") },
              { paramString("var", "Name", "affection"), paramOption("op", "Operation", "set", { "set", "add" }), paramString("value", "Value", "0") } },
            { "branch", "If", "Logic", "Branches with a typed expression; connect both results.", ImColor(232, 178, 92),
              { in("In") }, { out("True"), out("False") },
              { paramString("lhs", "Variable", "affection"),
                paramOption("operator", "Operator", ">=", { "==", "!=", "<", "<=", ">", ">=" }),
                paramString("rhs", "Compare To", "1") } },
            { "lua", "Lua Command", "Logic", "Runs a Lua hook command.", ImColor(232, 178, 92), { in("In") }, { out("Out") }, { paramString("fn", "Function", "PXEditorTransition"), paramString("args", "Arguments", "style=\"fade\" speed=\"10\"") } },
            { "transition",
              "BG Transition",
              "Stage",
              "Switches the background through a grayscale rule image.",
              ImColor(91, 209, 244),
              { in("In"), in("Image", PinType::Asset, "file"), in("Rule", PinType::Asset, "rule") },
              { out("Out") },
              { paramString("file", "New Background", "", ParamType::Asset),
                paramString("rule", "Rule Image", "", ParamType::Asset),
                paramInt("time", "Duration (ms)", 600, 50, 5000),
                paramInt("vague", "Band (vague)", 64, 1, 255) } },
            { "animate_actor",
              "Animate",
              "Stage",
              "Tweens an actor or layer to the given pose ([anim]).",
              ImColor(91, 209, 244),
              { in("In") },
              { out("Out") },
              { paramString("target", "Target", ""),
                paramFloat("x", "Offset X", 0.0f, -1920.0f, 1920.0f),
                paramFloat("y", "Offset Y", 0.0f, -1080.0f, 1080.0f),
                paramFloat("scale", "Scale", 1.0f, 0.1f, 4.0f),
                paramInt("alpha", "Alpha", 255, 0, 255),
                paramInt("duration", "Duration (ms)", 600, 0, 10000),
                paramOption("ease", "Easing", "outCubic",
                            { "linear", "inQuad", "outQuad", "inOutQuad", "inCubic", "outCubic",
                              "inOutCubic", "inQuart", "outQuart", "inOutQuart", "inSine",
                              "outSine", "inOutSine", "inExpo", "outExpo", "inOutExpo", "inBack",
                              "outBack", "inOutBack", "inElastic", "outElastic", "inBounce",
                              "outBounce", "smoothstep" }),
                paramBool("wait", "Wait for finish", false) } },
            { "spawn_ui", "UI Route", "UI", "Pushes, replaces, or opens a typed UI route.", ImColor(88, 230, 184), { in("In") }, { out("Out") }, { paramOption("ui", "Route", "title", {}), paramOption("operation", "Operation", "replace", {"push", "replace", "modal", "back"}) } },
            { "stop_bgm", "Stop BGM", "Audio", "Stops background music.", ImColor(255, 126, 95), { in("In") }, { out("Out") }, {} },
            { "string", "String", "Literals", "Reusable text value.", ImColor(160, 132, 255), {}, { out("Value", PinType::String, "value") }, { paramString("value", "Value", "text") } },
            { "asset", "Asset", "Literals", "Reusable asset reference.", ImColor(67, 186, 255), {}, { out("Path", PinType::Asset, "path") }, { paramString("path", "Path", "", ParamType::Asset) } },
            { "bool", "Bool", "Literals", "Reusable boolean.", ImColor(255, 104, 104), {}, { out("Value", PinType::Bool, "value") }, { paramBool("value", "Value", true) } },
        };
    }
    if (m_kind == GraphKind::Scenario) {
        for (const CustomCommandDef& cmd : m_customCommands) {
            NodeTemplate tpl;
            tpl.type = cmd.name;
            tpl.title = cmd.name;
            tpl.category = cmd.category.empty() ? "Custom" : cmd.category;
            tpl.description = cmd.description.empty() ? ("Custom Lua command (" + cmd.sourceFile + ")") : cmd.description;
            tpl.accent = ImColor(214, 122, 255);
            tpl.inputs = { in("In") };
            tpl.outputs = { out("Out") };
            for (const CustomCommandParam& p : cmd.params) {
                const std::string label = p.label.empty() ? p.key : p.label;
                if (!p.options.empty()) tpl.defaults.push_back(paramOption(p.key, label,
                    p.defaultValue.empty() ? p.options.front() : p.defaultValue, p.options));
                else if (p.type == "bool") tpl.defaults.push_back(paramBool(p.key, label,
                    p.defaultValue == "true" || p.defaultValue == "1"));
                else if (p.type == "int") { int value=0;try{value=std::stoi(p.defaultValue);}catch(...){}tpl.defaults.push_back(paramInt(p.key,label,value,-100000,100000)); }
                else if (p.type == "number") { float value=0.0f;try{value=std::stof(p.defaultValue);}catch(...){}tpl.defaults.push_back(paramFloat(p.key,label,value,-100000.0f,100000.0f)); }
                else tpl.defaults.push_back(paramString(p.key, label, p.defaultValue,
                    p.type == "resource" ? ParamType::Asset : ParamType::String,
                    p.type == "expression" || p.type == "map" || p.type == "list"));
            }
            m_library.push_back(std::move(tpl));
        }

        // CommandRegistry is the executable/schema source of truth.  The
        // original PDS canvas keeps its hand-crafted VN nodes above, while
        // commands without a specialised widget still get a fully typed node.
        for (const auto& descriptor : vn::CommandRegistry::Global().Descriptors()) {
            if (FindTemplate(descriptor.id)) continue;
            NodeTemplate node;
            node.type = descriptor.id;
            node.title = descriptor.displayName.empty() ? descriptor.id : descriptor.displayName;
            node.category = descriptor.category.empty() ? "Other" : descriptor.category;
            node.description = "Typed " + descriptor.id + " command";
            node.accent = ImColor(125, 165, 220);
            node.inputs = {in("In")};
            node.outputs = {out("Out")};
            for (const auto& parameter : descriptor.parameters) {
                const std::string label = parameter.label.empty() ? parameter.name : parameter.label;
                if (!parameter.options.empty()) {
                    node.defaults.push_back(paramOption(parameter.name, label,
                                                        parameter.options.front(), parameter.options));
                } else if (parameter.type == VariantType::Bool) {
                    node.defaults.push_back(paramBool(parameter.name, label, false));
                } else if (parameter.type == VariantType::Integer) {
                    const bool alpha = parameter.name == "alpha";
                    node.defaults.push_back(paramInt(parameter.name, label, 0,
                                                     alpha ? 0 : -100000,
                                                     alpha ? 255 : 600000));
                } else if (parameter.type == VariantType::Number) {
                    node.defaults.push_back(paramFloat(parameter.name, label, 0.0f,
                                                       -100000.0f, 100000.0f));
                } else {
                    const bool resource = parameter.type == VariantType::ResourceRef;
                    const bool multiline = parameter.widget == vn::CommandEditorWidget::Multiline ||
                                           parameter.widget == vn::CommandEditorWidget::Expression;
                    node.defaults.push_back(paramString(parameter.name, label, {},
                                                        resource ? ParamType::Asset : ParamType::String,
                                                        multiline));
                }
            }
            m_library.push_back(std::move(node));
        }
        if (m_project) {
            if (auto* routeNode = const_cast<NodeTemplate*>(FindTemplate("spawn_ui"))) {
                if (auto parameter = std::find_if(routeNode->defaults.begin(), routeNode->defaults.end(),
                    [](const Parameter& field) { return field.key == "ui"; });
                    parameter != routeNode->defaults.end()) {
                    parameter->options.clear();
                    for (const auto& route : m_project->manifest.routes) parameter->options.push_back(route.id);
                    if (!parameter->options.empty()) parameter->stringValue = parameter->options.front();
                }
            }
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
    m_groups.clear();
    m_unknownNodes.clear();
    m_unknownLinks.clear();
    m_editHistory.Clear();
    m_undoArmed = false;
    m_undoGestureDirty = false;
    m_nextId = 1;
    m_selectedNodeId = 0;
    m_selectedLinkId = 0;
    m_dirty = false;

    const fs::path path = DocumentPath();
    if (path.empty()) {
        return;
    }

    if (m_kind == GraphKind::Scenario) {
        if (ImportScenario(path)) {
            m_dirty = false;
            m_loaded = true;
            m_navigateCountdown = 3;
            Log("Loaded Scenario document: " + path.string());
            return;
        }
        if (fs::exists(path)) {
            // The file exists but could not be read; never overwrite it with a
            // seeded default graph.
            Log("Could not read Scenario document (file left untouched): " + path.string());
            m_loaded = true;
            return;
        }
        SeedDefaultGraph();
        (void)Save();
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
    (void)Save();
    m_loaded = true;
    m_navigateCountdown = 3;
}

void NodeGraphEditor::Reload() {
    m_loaded = false;
    LoadOrCreate();
}

void NodeGraphEditor::ReloadIfOpen(const std::string& script) {
    if (m_documentRuntimePath.empty() || script.empty()) {
        return;
    }
    const std::string current =
        std::filesystem::path(m_documentRuntimePath).filename().string();
    const std::string target = std::filesystem::path(script).filename().string();
    if (current != target) {
        return;
    }
    if (m_dirty) {
        Log("Script changed on disk by the Flow editor; save or reload \"" + current +
            "\" to sync.");
        return;
    }
    Reload();
}

bool NodeGraphEditor::OpenDocument(const std::string& runtimePath) {
    if (m_kind != GraphKind::Scenario || !m_project || !m_project->IsOpen()) {
        return false;
    }
    const std::string normalized = NormalizeScenarioRuntimePath(runtimePath);
    if (normalized.empty()) {
        return false;
    }
    if (m_dirty) {
        (void)Save();
    }
    m_documentRuntimePath = normalized;
    m_documentPathInput = normalized;
    m_loaded = false;
    LoadOrCreate();
    return true;
}

bool NodeGraphEditor::NewDocument(const std::string& runtimePath) {
    if (m_kind != GraphKind::Scenario || !m_project || !m_project->IsOpen()) {
        return false;
    }
    const std::string normalized = NormalizeScenarioRuntimePath(runtimePath);
    if (normalized.empty()) {
        return false;
    }
    if (fs::exists(m_project->root / fs::path(normalized))) {
        Log("Scenario already exists; opening it: \"" + normalized + "\"");
        return OpenDocument(runtimePath);
    }
    if (m_dirty) {
        (void)Save();
    }
    m_documentRuntimePath = normalized;
    m_documentPathInput = normalized;
    m_editDocumentId = Uuid::Random();
    SeedDefaultGraph();
    (void)Save();
    m_loaded = true;
    m_navigateCountdown = 3;
    return true;
}

void NodeGraphEditor::LoadGraph(const Json& json) {
    m_nodes.clear();
    m_links.clear();
    m_groups.clear();
    m_unknownNodes.clear();
    m_unknownLinks.clear();
    m_nextId = json.value("nextId", 1);

    for (const Json& item : json.value("nodes", Json::array())) {
        Node node;
        node.id = item.value("id", m_nextId++);
        if(const auto stable=Uuid::Parse(item.value("stableId",std::string{})))node.stableId=*stable;else node.stableId=Uuid::Random();
        node.type = item.value("type", "");
        const NodeTemplate* definition = FindTemplate(node.type);
        if (!definition) {
            // Unknown template (e.g. a removed custom command): keep the raw
            // JSON so it survives the next save instead of vanishing.
            m_unknownNodes.push_back(item);
            m_nextId = std::max(m_nextId, node.id + 1);
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
            ApplyParamsJson(node, item["params"]);
        }
        if (node.type == "dialogue") {
            for (const auto& encoded : item.value("lineIds", Json::array()))
                if (encoded.is_string())
                    if (const auto id = Uuid::Parse(encoded.get<std::string>())) node.dialogueLineIds.push_back(*id);
            const auto* text = FindParameter(node, "text");
            const std::size_t count = text ? SplitTextLines(text->stringValue).size() : 1;
            while (node.dialogueLineIds.size() < count) node.dialogueLineIds.push_back(Uuid::Random());
            node.dialogueLineIds.resize(count);
        }
        m_nodes.push_back(std::move(node));
    }

    for (const Json& item : json.value("groups", Json::array())) {
        GroupNode group;
        group.id = item.value("id", m_nextId++);
        group.title = item.value("title", std::string{ "Group" });
        if (item.contains("position") && item["position"].is_array() &&
            item["position"].size() >= 2) {
            group.position = ImVec2(item["position"][0].get<float>(),
                                    item["position"][1].get<float>());
        }
        if (item.contains("size") && item["size"].is_array() && item["size"].size() >= 2) {
            group.size = ImVec2(item["size"][0].get<float>(), item["size"][1].get<float>());
        }
        group.positionInitialized = true;
        m_nextId = std::max(m_nextId, group.id + 1);
        m_groups.push_back(std::move(group));
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
        if(const auto stable=Uuid::Parse(item.value("stableId",std::string{})))link.stableId=*stable;else link.stableId=Uuid::Random();
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
        } else {
            m_unknownLinks.push_back(item);
        }
    }
    if (!m_unknownNodes.empty()) {
        Log(std::to_string(m_unknownNodes.size()) +
            " node(s) use missing templates; they are hidden but preserved on save.");
    }
    m_dirty = false;
}

Json NodeGraphEditor::SaveGraph() const {
    Json graph;
    graph["version"] = 4;
    graph["graphKind"] = "scenario";
    graph["output"] = CurrentRuntimePath();
    graph["nextId"] = m_nextId;
    graph["comments"] = Json::array();
    graph["groups"] = Json::array();
    graph["nodes"] = Json::array();
    graph["links"] = Json::array();

    for (const Node& node : m_nodes) {
        Json item;
        item["id"] = node.id;
        item["stableId"] = node.stableId.ToString();
        item["type"] = node.type;
        item["position"] = Json::array({ node.position.x, node.position.y });
        item["params"] = ParamsToJson(node);
        if (!node.dialogueLineIds.empty()) {
            item["lineIds"] = Json::array();
            for (const auto& id : node.dialogueLineIds) item["lineIds"].push_back(id.ToString());
        }
        graph["nodes"].push_back(std::move(item));
    }
    for (const GroupNode& group : m_groups) {
        graph["groups"].push_back(Json{
            { "id", group.id },
            { "title", group.title },
            { "position", Json::array({ group.position.x, group.position.y }) },
            { "size", Json::array({ group.size.x, group.size.y }) },
        });
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
        graph["links"].push_back(
            Json{
                { "id", link.id },
                { "stableId", link.stableId.ToString() },
                { "startPinId", link.startPinId },
                { "endPinId", link.endPinId },
                { "from", pinIndex(link.startPinId) },
                { "to", pinIndex(link.endPinId) },
            }
        );
    }
    for (const Json& item : m_unknownNodes) {
        graph["nodes"].push_back(item);
    }
    for (const Json& item : m_unknownLinks) {
        graph["links"].push_back(item);
    }
    return graph;
}

bool NodeGraphEditor::Save() {
    const fs::path path = DocumentPath();
    if (path.empty()) {
        return false;
    }
    UpdateNodePositions();
    fs::create_directories(path.parent_path());
    try {
        const std::string content=Compile();
        if (content.empty()) {
            Log("Scenario has blocking diagnostics and was not saved.");
            return false;
        }
        const Status written=io::AtomicFile::WriteText(path,content);if(!written){for(const auto& diagnostic:written.Diagnostics())diag::Emit(diagnostic);return false;}
        vn::scenario::ScenarioLayoutDocument layout;layout.scenario=m_editDocumentId;
        for(const auto& node:m_nodes)if(node.type!="scenario_start")layout.nodes.push_back({node.type=="dialogue"&&!node.dialogueLineIds.empty()?node.dialogueLineIds.front():node.stableId,{node.position.x,node.position.y},{}, {}});
        auto layoutPath=path;layoutPath.replace_extension(".pxlayout");const Status layoutWritten=io::AtomicFile::WriteText(layoutPath,vn::scenario::WriteScenarioLayout(layout));if(!layoutWritten){for(const auto& diagnostic:layoutWritten.Diagnostics())diag::Emit(diagnostic);return false;}
        if(m_identityRegistrar){const Status identities=m_identityRegistrar({path,layoutPath});if(!identities){for(const auto& diagnostic:identities.Diagnostics())diag::Emit(diagnostic);return false;}}
        m_dirty = false;
        m_editHistory.MarkSaved();m_undoBaseline=SaveGraph();m_undoGestureDirty=false;m_undoArmed=true;
        m_loaded = true;
        Log("Saved Scenario and layout: " + path.string());
        return true;
    } catch (const std::exception& exception) {
        Log(std::string("Save failed: ") + exception.what());
        diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8005",
            .category="Editor.Document",.message="無法儲存文件",.details=exception.what()};
        diagnostic.source.path=path.generic_string();diag::Emit(std::move(diagnostic));
        return false;
    }
}

void NodeGraphEditor::SeedDefaultGraph() {
    m_nodes.clear();
    m_links.clear();
    m_groups.clear();
    m_nextId = 1;
    if (m_kind == GraphKind::Scenario) {
        std::vector<std::string> types{ "scenario_start", "chapter", "dialogue" };
        for (size_t i = 0; i < types.size(); ++i) {
            AddNode(types[i], ImVec2(static_cast<float>(i) * 280.0f, (i % 2 == 0) ? 0.0f : 170.0f));
        }
    }
    CreateDefaultLinkChain();
    m_dirty = true;
}

bool NodeGraphEditor::ImportScenario(const fs::path& path) {
    if (!fs::exists(path)) {
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream text; text << in.rdbuf();
    const auto parsed = vn::scenario::ParseScenario(text.str(), path.generic_string());
    if (!parsed) { for (const auto& diagnostic : parsed.Diagnostics()) diag::Emit(diagnostic); return false; }
    return ImportScenarioDocument(parsed.Value());
}

bool NodeGraphEditor::ImportScenarioText(const std::string& text) {
    if (m_kind != GraphKind::Scenario) {
        return false;
    }
    const auto parsed = vn::scenario::ParseScenario(text, CurrentRuntimePath());
    if (!parsed) { for (const auto& diagnostic : parsed.Diagnostics()) diag::Emit(diagnostic); return false; }
    MarkDirty();
    if (!ImportScenarioDocument(parsed.Value())) return false;
    m_loaded = true;
    m_navigateCountdown = 3;
    return true;
}

bool NodeGraphEditor::ImportScenarioDocument(const vn::scenario::ScenarioDocument& document) {
    m_editDocumentId = document.id;
    m_nodes.clear(); m_links.clear(); m_groups.clear(); m_unknownNodes.clear();
    m_unknownLinks.clear(); m_nextId = 1;
    Node* start = AddNode("scenario_start", ImVec2(0, 0));
    if (!start) return false;
    const int startNodeId = start->id;
    if (auto* title = FindParameter(*start, "title")) title->stringValue = document.name;

    std::unordered_map<Uuid, Vec2, UuidHash> positions;
    auto layoutPath = DocumentPath(); layoutPath.replace_extension(".pxlayout");
    if (std::ifstream stream(layoutPath, std::ios::binary); stream) {
        std::ostringstream text; text << stream.rdbuf();
        const auto layout = vn::scenario::ParseScenarioLayout(text.str(), layoutPath.generic_string());
        if (layout && layout.Value().scenario == document.id)
            for (const auto& item : layout.Value().nodes) positions[item.node] = item.position;
    }
    const auto editorType = [](const vn::scenario::ScenarioNode& source) {
        if (source.command == "say" || source.command == "text") return std::string("dialogue");
        if (source.command == "bg") return source.parameters.contains("rule") ? std::string("transition") : std::string("background");
        if (source.command == "char") return std::string("character");
        if (source.command == "var") return std::string("variable");
        if (source.command == "anim" || source.command == "tween") return std::string("animate_actor");
        if (source.command == "stopbgm") return std::string("stop_bgm");
        if (source.command == "route") return std::string("spawn_ui");
        return source.command;
    };
    std::unordered_map<Uuid, int, UuidHash> nodeIds;
    std::unordered_map<std::string, Node*> dialogueBlocks;
    std::size_t visualIndex = 1;
    for (const auto& source : document.nodes) {
        if (source.command == "say" || source.command == "text") {
            const auto block = source.parameters.find("blockId");
            const auto* blockId = block != source.parameters.end() ? block->second.TryGet<std::string>() : nullptr;
            if (blockId && !blockId->empty()) {
                if (const auto found = dialogueBlocks.find(*blockId); found != dialogueBlocks.end()) {
                    Node* existing = found->second;
                    const auto value = source.parameters.find("value");
                    const auto* line = value != source.parameters.end() ? value->second.TryGet<std::string>() : nullptr;
                    if (auto* text = FindParameter(*existing, "text")) text->stringValue += "\n" + (line ? *line : std::string{});
                    if (auto* voices = FindParameter(*existing, "voices")) {
                        std::string voicePath;
                        const auto voice = source.parameters.find("voice");
                        if (voice != source.parameters.end())
                            if (const auto* reference = voice->second.TryGet<ResourceRefValue>()) voicePath = reference->lastKnownPath;
                        voices->stringValue += "\n" + voicePath;
                    }
                    existing->dialogueLineIds.push_back(source.id);
                    nodeIds[source.id] = existing->id;
                    continue;
                }
            }
        }
        const auto position = positions.find(source.id);
        const ImVec2 canvas = position == positions.end()
            ? ImVec2(static_cast<float>(visualIndex % 4) * 620.0f,
                     static_cast<float>(visualIndex / 4) * 320.0f)
            : ImVec2(position->second.x, position->second.y);
        Node* node = AddNode(editorType(source), canvas);
        if (!node) { Log("Scenario command has no Node template: " + source.command); continue; }
        node->stableId = source.id; nodeIds[source.id] = node->id; ++visualIndex;
        if (node->type == "dialogue") node->dialogueLineIds = {source.id};
        if (node->type == "dialogue") {
            const auto block = source.parameters.find("blockId");
            if (block != source.parameters.end())
                if (const auto* blockId = block->second.TryGet<std::string>(); blockId && !blockId->empty())
                    dialogueBlocks[*blockId] = node;
        }
        for (const auto& [sourceName, value] : source.parameters) {
            std::string name = sourceName;
            if (node->type == "dialogue" && sourceName == "value") name = "text";
            if (node->type == "dialogue" && sourceName == "voice") name = "voices";
            if (node->type == "variable" && sourceName == "name") name = "var";
            if (node->type == "spawn_ui" && sourceName == "route") name = "ui";
            Parameter* parameter = FindParameter(*node, name);
            if (!parameter) continue;
            if (const auto* booleanValue = value.TryGet<bool>()) parameter->boolValue = *booleanValue;
            else if (const auto* integerValue = value.TryGet<std::int64_t>()) { parameter->intValue = static_cast<int>(*integerValue); parameter->stringValue = std::to_string(*integerValue); }
            else if (const auto* numberValue = value.TryGet<double>()) parameter->floatValue = static_cast<float>(*numberValue);
            else if (const auto* stringValue = value.TryGet<std::string>()) parameter->stringValue = *stringValue;
            else if (const auto* resourceValue = value.TryGet<ResourceRefValue>()) parameter->stringValue = resourceValue->lastKnownPath;
        }
        if (node->type == "branch") {
            const auto expression = source.parameters.find("expression");
            if (expression != source.parameters.end()) {
                auto parsed = vn::ExpressionFromValue(expression->second);
                if (parsed && parsed.Value().kind == vn::ExpressionKind::Binary &&
                    parsed.Value().left && parsed.Value().right &&
                    parsed.Value().left->kind == vn::ExpressionKind::Variable &&
                    parsed.Value().right->kind == vn::ExpressionKind::Literal) {
                    if (auto* lhs = FindParameter(*node, "lhs")) lhs->stringValue = parsed.Value().left->variable;
                    if (auto* op = FindParameter(*node, "operator")) op->stringValue = vn::ToString(parsed.Value().op);
                    if (auto* rhs = FindParameter(*node, "rhs")) {
                        const Variant& literal = parsed.Value().right->literal;
                        if (const auto* text = literal.TryGet<std::string>()) rhs->stringValue = *text;
                        else if (const auto* integer = literal.TryGet<std::int64_t>()) rhs->stringValue = std::to_string(*integer);
                        else if (const auto* number = literal.TryGet<double>()) rhs->stringValue = std::to_string(*number);
                        else if (const auto* boolean = literal.TryGet<bool>()) rhs->stringValue = *boolean ? "true" : "false";
                    }
                }
            }
        }
        if (node->type == "variable") {
            if (source.parameters.contains("add")) if(auto* op=FindParameter(*node,"op"))op->stringValue="add";
            if (const auto found=source.parameters.find("add");found!=source.parameters.end())
                if (const auto* number=found->second.TryGet<double>())if(auto* output=FindParameter(*node,"value"))output->stringValue=std::to_string(*number);
        }
    }
    for (const auto& edge : document.edges) {
        const auto from=nodeIds.find(edge.fromNode),to=nodeIds.find(edge.toNode);
        if(from==nodeIds.end()||to==nodeIds.end())continue;
        Node* source=FindNode(from->second);Node* target=FindNode(to->second);
        if(!source||!target||source->outputs.empty()||target->inputs.empty())continue;
        if(source==target)continue; // internal lines of one DialogueBlock
        std::size_t outputIndex = 0;
        if (edge.fromPort != "flow") {
            const auto output = std::find_if(source->outputs.begin(), source->outputs.end(),
                [&edge](const Pin& pin) { return Lower(pin.label) == edge.fromPort; });
            outputIndex = output == source->outputs.end()
                ? std::min<std::size_t>(1, source->outputs.size() - 1)
                : static_cast<std::size_t>(std::distance(source->outputs.begin(), output));
        }
        AddLink(source->outputs[outputIndex].id,target->inputs.front().id);
        if(!m_links.empty())m_links.back().stableId=edge.id;
    }
    if (const auto entry=nodeIds.find(document.entry);entry!=nodeIds.end()) {
        Node* source=FindNode(startNodeId);Node* target=FindNode(entry->second);
        if(source&&target&&!source->outputs.empty()&&!target->inputs.empty())AddLink(source->outputs.front().id,target->inputs.front().id);
    }
    m_dirty = false;
    return !document.nodes.empty();
}

NodeGraphEditor::Node* NodeGraphEditor::AddNode(const std::string& type, ImVec2 position) {
    const NodeTemplate* definition = FindTemplate(type);
    if (!definition) {
        return nullptr;
    }
    Node node;
    node.id = m_nextId++;
    node.stableId = Uuid::Random();
    node.type = type;
    node.position = position;
    node.positionInitialized = true;
    node.parameters = definition->defaults;
    if (type == "dialogue") node.dialogueLineIds.push_back(Uuid::Random());
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
    const auto groupIt = std::find_if(m_groups.begin(), m_groups.end(),
                                      [&](const GroupNode& group) { return group.id == id; });
    if (groupIt != m_groups.end()) {
        m_groups.erase(groupIt);
        MarkDirty();
        return;
    }
    m_links.erase(
        std::remove_if(
            m_links.begin(),
            m_links.end(),
            [&](const Link& link) {
                const Pin* start = FindPin(link.startPinId);
                const Pin* end = FindPin(link.endPinId);
                return (start && start->nodeId == id) || (end && end->nodeId == id);
            }
        ),
        m_links.end()
    );
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
    m_links.push_back(Link{ m_nextId++, startPinId, endPinId, Uuid::Random() });
    MarkDirty();
}

void NodeGraphEditor::RemoveLink(int id) {
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const Link& link) { return link.id == id; }), m_links.end());
    MarkDirty();
}

void NodeGraphEditor::DeleteSelection() {
    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) {
        return;
    }

    std::vector<ed::LinkId> links(static_cast<size_t>(count));
    const int linkCount = ed::GetSelectedLinks(links.data(), count);
    for (int i = 0; i < linkCount; ++i) {
        RemoveLink(static_cast<int>(links[static_cast<size_t>(i)].Get()));
    }

    std::vector<ed::NodeId> nodes(static_cast<size_t>(count));
    const int nodeCount = ed::GetSelectedNodes(nodes.data(), count);
    for (int i = 0; i < nodeCount; ++i) {
        RemoveNode(static_cast<int>(nodes[static_cast<size_t>(i)].Get()));
    }

    ed::ClearSelection();
}

void NodeGraphEditor::CreateDefaultLinkChain() {
    for (size_t i = 0; i + 1 < m_nodes.size(); ++i) {
        if (m_nodes[i].outputs.empty() || m_nodes[i + 1].inputs.empty()) {
            continue;
        }
        AddLink(m_nodes[i].outputs.front().id, m_nodes[i + 1].inputs.front().id);
    }
}

Json NodeGraphEditor::ParamsToJson(const Node& node) const {
    Json params = Json::object();
    for (const Parameter& parameter : node.parameters) {
        switch (parameter.type) {
            case ParamType::Bool:
                params[parameter.key] = parameter.boolValue;
                break;
            case ParamType::Int:
                params[parameter.key] = parameter.intValue;
                break;
            case ParamType::Float:
                params[parameter.key] = parameter.floatValue;
                break;
            case ParamType::Color:
                params[parameter.key] = ColorToJson(parameter.colorValue);
                break;
            case ParamType::String:
            case ParamType::Asset:
            case ParamType::Option:
                params[parameter.key] = parameter.stringValue;
                break;
        }
    }
    return params;
}

void NodeGraphEditor::ApplyParamsJson(Node& node, const Json& params) {
    for (Parameter& parameter : node.parameters) {
        if (!params.contains(parameter.key)) {
            continue;
        }
        const Json& value = params[parameter.key];
        try {
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
        } catch (...) {
            // keep the default when the stored value has the wrong type
        }
    }
}

void NodeGraphEditor::CopySelection() {
    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) {
        return;
    }
    std::vector<ed::NodeId> selected(static_cast<std::size_t>(count));
    const int nodeCount = ed::GetSelectedNodes(selected.data(), count);
    std::vector<const Node*> nodes;
    std::unordered_set<int> ids;
    for (int i = 0; i < nodeCount; ++i) {
        if (const Node* node = FindNode(static_cast<int>(selected[static_cast<std::size_t>(i)].Get()))) {
            nodes.push_back(node);
            ids.insert(node->id);
        }
    }
    if (nodes.empty()) {
        return;
    }
    UpdateNodePositions();

    ImVec2 origin(nodes.front()->position);
    for (const Node* node : nodes) {
        origin.x = std::min(origin.x, node->position.x);
        origin.y = std::min(origin.y, node->position.y);
    }

    Json clip;
    clip["originX"] = origin.x;
    clip["originY"] = origin.y;
    clip["nodes"] = Json::array();
    clip["links"] = Json::array();
    for (const Node* node : nodes) {
        Json item;
        item["type"] = node->type;
        item["x"] = node->position.x - origin.x;
        item["y"] = node->position.y - origin.y;
        item["params"] = ParamsToJson(*node);
        clip["nodes"].push_back(std::move(item));
    }
    const auto nodeIndex = [&](int nodeId) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i]->id == nodeId) return static_cast<int>(i);
        }
        return -1;
    };
    for (const Link& link : m_links) {
        const Pin* start = FindPin(link.startPinId);
        const Pin* end = FindPin(link.endPinId);
        if (!start || !end || ids.count(start->nodeId) == 0 || ids.count(end->nodeId) == 0) {
            continue;
        }
        const Node* from = FindNode(start->nodeId);
        const Node* to = FindNode(end->nodeId);
        int fromPin = -1, toPin = -1;
        for (std::size_t i = 0; i < from->outputs.size(); ++i) {
            if (from->outputs[i].id == link.startPinId) fromPin = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < to->inputs.size(); ++i) {
            if (to->inputs[i].id == link.endPinId) toPin = static_cast<int>(i);
        }
        if (fromPin >= 0 && toPin >= 0) {
            clip["links"].push_back(Json{ { "fromNode", nodeIndex(start->nodeId) },
                                          { "fromPin", fromPin },
                                          { "toNode", nodeIndex(end->nodeId) },
                                          { "toPin", toPin } });
        }
    }
    m_clipboard = std::move(clip);
    Log(std::to_string(nodes.size()) + " node(s) copied.");
}

void NodeGraphEditor::PasteClipboard(ImVec2 canvasPosition) {
    if (!m_clipboard.is_object() || !m_clipboard.contains("nodes") ||
        m_clipboard["nodes"].empty()) {
        return;
    }
    std::vector<int> createdIds;
    for (const Json& item : m_clipboard["nodes"]) {
        Node* node = AddNode(item.value("type", std::string{}),
                             ImVec2(canvasPosition.x + item.value("x", 0.0f),
                                    canvasPosition.y + item.value("y", 0.0f)));
        if (node && item.contains("params")) {
            ApplyParamsJson(*node, item["params"]);
        }
        createdIds.push_back(node ? node->id : 0);
    }
    for (const Json& linkItem : m_clipboard.value("links", Json::array())) {
        const int fromIdx = linkItem.value("fromNode", -1);
        const int toIdx = linkItem.value("toNode", -1);
        if (fromIdx < 0 || toIdx < 0 || fromIdx >= static_cast<int>(createdIds.size()) ||
            toIdx >= static_cast<int>(createdIds.size())) {
            continue;
        }
        Node* from = FindNode(createdIds[static_cast<std::size_t>(fromIdx)]);
        Node* to = FindNode(createdIds[static_cast<std::size_t>(toIdx)]);
        const int fromPin = linkItem.value("fromPin", -1);
        const int toPin = linkItem.value("toPin", -1);
        if (from && to && fromPin >= 0 && toPin >= 0 &&
            fromPin < static_cast<int>(from->outputs.size()) &&
            toPin < static_cast<int>(to->inputs.size())) {
            AddLink(from->outputs[static_cast<std::size_t>(fromPin)].id,
                    to->inputs[static_cast<std::size_t>(toPin)].id);
        }
    }
    ed::ClearSelection();
    for (int id : createdIds) {
        if (id != 0) {
            ed::SelectNode(ed::NodeId(id), /*append=*/true);
        }
    }
    MarkDirty();
}

void NodeGraphEditor::DuplicateSelection() {
    CopySelection();
    if (!m_clipboard.is_object() || !m_clipboard.contains("originX")) {
        return;
    }
    PasteClipboard(ImVec2(m_clipboard.value("originX", 0.0f) + 48.0f,
                          m_clipboard.value("originY", 0.0f) + 48.0f));
}

NodeGraphEditor::GroupNode* NodeGraphEditor::FindGroup(int id) {
    auto it = std::find_if(m_groups.begin(), m_groups.end(),
                           [&](const GroupNode& group) { return group.id == id; });
    return it == m_groups.end() ? nullptr : &(*it);
}

void NodeGraphEditor::AddGroup(ImVec2 position) {
    GroupNode group;
    group.id = m_nextId++;
    group.position = position;
    m_groups.push_back(std::move(group));
    MarkDirty();
}

void NodeGraphEditor::RenderGroupNode(GroupNode& group) {
    if (group.positionInitialized) {
        ed::SetNodePosition(ed::NodeId(group.id), group.position);
        group.positionInitialized = false;
    }
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImColor(255, 255, 255, 22));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(255, 255, 255, 64));
    ed::BeginNode(ed::NodeId(group.id));
    ImGui::PushID(group.id);
    ImGui::TextColored(ImVec4(0.85f, 0.90f, 1.0f, 0.85f), "%s", group.title.c_str());
    ed::Group(group.size);
    ImGui::PopID();
    ed::EndNode();
    const ImVec2 nodeSize = ImGui::GetItemRectSize();
    group.decoration = ImVec2(nodeSize.x - group.size.x, nodeSize.y - group.size.y);
    ed::PopStyleColor(2);
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
    for (GroupNode& group : m_groups) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(group.id));
        if (std::abs(position.x - group.position.x) > 0.25f ||
            std::abs(position.y - group.position.y) > 0.25f) {
            group.position = position;
            MarkDirty();
        }
        // The node is the group area plus its title decoration (measured at
        // render time), so subtracting it recovers the resized group size
        // without feedback growth.
        const ImVec2 nodeSize = ed::GetNodeSize(ed::NodeId(group.id));
        const ImVec2 size(nodeSize.x - group.decoration.x, nodeSize.y - group.decoration.y);
        if (size.x > 60.0f && size.y > 40.0f &&
            (std::abs(size.x - group.size.x) > 0.5f || std::abs(size.y - group.size.y) > 0.5f)) {
            group.size = size;
            MarkDirty();
        }
    }
}

void NodeGraphEditor::MarkDirty() {
    m_undoGestureDirty = true;
    m_dirty = true;
    ++m_revision;
}

void NodeGraphEditor::RelocateIfOpen(const std::string& oldRuntimePath,
                                     const std::string& newRuntimePath) {
    const std::string oldNormalized = NormalizeScenarioRuntimePath(oldRuntimePath);
    if (m_kind == GraphKind::Scenario &&
        (m_documentRuntimePath == oldNormalized ||
         fs::path(m_documentRuntimePath).filename() == fs::path(oldNormalized).filename())) {
        m_documentRuntimePath = NormalizeScenarioRuntimePath(newRuntimePath);
    }
}

void NodeGraphEditor::Undo() {
    if(!m_editHistory.CanUndo())return;const Status status=m_editHistory.Undo();if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);else{m_dirty=m_editHistory.Dirty();++m_revision;}
}

void NodeGraphEditor::Redo() {
    if(!m_editHistory.CanRedo())return;const Status status=m_editHistory.Redo();if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);else{m_dirty=m_editHistory.Dirty();++m_revision;}
}

void NodeGraphEditor::RestoreGraph(const Json& snapshot) {
    LoadGraph(snapshot);
    m_dirty = true;
    m_loaded = true;
    m_undoBaseline=SaveGraph();m_undoArmed=true;m_undoGestureDirty=false;
}

Result<Variant> NodeGraphEditor::ReadProperty(const Uuid& target,const std::string& property)const{
    if(target!=m_editDocumentId||property!="graph")return Result<Variant>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8001",.category="Editor.Graph",.message="Unknown graph edit target/property"});
    return Result<Variant>::Success(Variant(SaveGraph().dump()));
}
Status NodeGraphEditor::WriteProperty(const Uuid& target,const std::string& property,const Variant& value){
    if(target!=m_editDocumentId||property!="graph")return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8001",.category="Editor.Graph",.message="Unknown graph edit target/property"});
    const auto* text=value.TryGet<std::string>();if(!text)return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8002",.category="Editor.Graph",.message="Graph snapshot must be String"});
    Json json=Json::parse(*text,nullptr,false);if(json.is_discarded())return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8003",.category="Editor.Graph",.message="Graph snapshot is corrupt"});RestoreGraph(json);return Status::Ok();
}
Result<VariantObject> NodeGraphEditor::CaptureSubtree(const Uuid&)const{return Result<VariantObject>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8004",.category="Editor.Graph",.message="Graph document does not support subtree commands"});}
Status NodeGraphEditor::InsertSubtree(const Uuid&,std::size_t,const VariantObject&){return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8004",.category="Editor.Graph",.message="Graph document does not support subtree commands"});}
Result<VariantObject> NodeGraphEditor::RemoveSubtree(const Uuid&){return Result<VariantObject>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8004",.category="Editor.Graph",.message="Graph document does not support subtree commands"});}
Status NodeGraphEditor::Reparent(const Uuid&,const Uuid&,std::size_t){return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8004",.category="Editor.Graph",.message="Graph document does not support subtree commands"});}
Status NodeGraphEditor::MoveChild(const Uuid&,const Uuid&,std::size_t){return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXGRAPH8004",.category="Editor.Graph",.message="Graph document does not support child-order commands"});}

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
    ImGui::BeginChild((Title() + "Toolbar").c_str(), ImVec2(0, m_kind == GraphKind::Scenario ? 74.0f : 42.0f), false);
    if (m_kind == GraphKind::Scenario) {
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##scenario-document", "Content/Scenario/chapter.pxscenario", &m_documentPathInput);
        ImGui::SameLine();
        if (ImGui::Button("Open Scenario")) {
            if (m_dirty) {
                m_pendingDocAction = 1;
                m_pendingDocPath = m_documentPathInput;
            } else {
                OpenDocument(m_documentPathInput);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("New Scenario")) {
            if (m_dirty) {
                m_pendingDocAction = 2;
                m_pendingDocPath = m_documentPathInput;
            } else {
                NewDocument(m_documentPathInput);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", CurrentRuntimePath().c_str(), m_dirty ? " *" : "");
    }
    if (ImGui::Button("Save")) (void)Save();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) Reload();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame")) FrameSelection();
    ImGui::SameLine();
    if (ImGui::Button("Align X")) AlignSelection(false);
    ImGui::SameLine();
    if (ImGui::Button("Align Y")) AlignSelection(true);
    ImGui::SameLine();
    if (ImGui::Button("Compile")) Log("Compiled " + CurrentRuntimePath() + " (" + std::to_string(Compile().size()) + " bytes)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##node-search", "Search nodes...", &m_search);
    ImGui::SameLine();
    if (m_kind != GraphKind::Scenario) {
        ImGui::TextDisabled("%s%s", DocumentPath().filename().string().c_str(), m_dirty ? " *" : "");
    }
    RenderUnsavedChangesModal();
    ImGui::EndChild();
}

void NodeGraphEditor::RenderUnsavedChangesModal() {
    if (m_pendingDocAction != 0 && !ImGui::IsPopupOpen("Unsaved Changes")) {
        ImGui::OpenPopup("Unsaved Changes");
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::Text("\"%s\" has unsaved changes.", CurrentRuntimePath().c_str());
    ImGui::TextDisabled("Save them before switching documents?");
    ImGui::Separator();
    const auto proceed = [&] {
        const int action = m_pendingDocAction;
        const std::string path = m_pendingDocPath;
        m_pendingDocAction = 0;
        if (action == 1) {
            OpenDocument(path);
        } else {
            NewDocument(path);
        }
    };
    if (ImGui::Button("Save & Continue")) {
        (void)Save();
        proceed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard Changes")) {
        m_dirty = false;
        proceed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_pendingDocAction = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NodeGraphEditor::RenderGraph() {
    m_dropTargets.clear();
    ed::SetCurrentEditor(m_context);
    ed::Begin(Title().c_str(), ImVec2(0, 0));

    for (GroupNode& group : m_groups) {
        RenderGroupNode(group);
    }
    for (Node& node : m_nodes) {
        RenderNode(node);
    }
    for (const Link& link : m_links) {
        const Pin* start = FindPin(link.startPinId);
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPinId), ed::PinId(link.endPinId), start ? PinColor(start->type) : ImColor(255, 255, 255), 2.0f);
    }

    if (m_focusNodeId != 0) {
        ed::ClearSelection();
        ed::SelectNode(ed::NodeId(m_focusNodeId));
        m_selectedNodeId = m_focusNodeId;
        ed::NavigateToSelection(true, 0.25f);
        m_focusNodeId = 0;
    }

    HandleInteractions();
    RefreshSelection();
    UpdateNodePositions();
    if (m_navigateCountdown > 0 && --m_navigateCountdown == 0) {
        ed::NavigateToContent();
    }

    ed::Suspend();
    RenderMiniMap();
    ed::NodeId contextNodeId = 0;
    ed::LinkId contextLinkId = 0;
    if (ed::ShowNodeContextMenu(&contextNodeId)) {
        m_contextNodeId = static_cast<int>(contextNodeId.Get());
        ImGui::OpenPopup("Node Menu");
    }
    else if (ed::ShowLinkContextMenu(&contextLinkId)) {
        m_contextLinkId = static_cast<int>(contextLinkId.Get());
        ImGui::OpenPopup("Link Menu");
    }
    else if (ed::ShowBackgroundContextMenu()) {
        m_pendingPinId = 0;
        m_createPosition = ImGui::GetMousePos();
        m_createPopup = true;
        ImGui::OpenPopup("Create Node");
    }
    RenderCreatePopup();
    RenderNodeContextMenu();
    RenderInNodePopups();
    ed::Resume();

    ed::End();

    HandleAssetDrop(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // Re-arm undo capture once no edit gesture is in flight.
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Json current=SaveGraph();
        if(m_undoArmed&&m_undoGestureDirty&&current!=m_undoBaseline){
            auto command=std::make_unique<PropertyChangeCommand>("Edit story graph",m_editDocumentId,"graph",Variant(m_undoBaseline.dump()),Variant(current.dump()),std::chrono::steady_clock::now(),false);
            const Status status=m_editHistory.CommitApplied(std::move(command));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);
        }
        m_undoBaseline=current;m_undoArmed=true;m_undoGestureDirty=false;
    }
}

void NodeGraphEditor::RecordDropTarget(int nodeId, const std::string& paramKey, int lineIndex) {
    if (nodeId == 0) {
        return;
    }
    AssetDropTarget target;
    target.nodeId = nodeId;
    target.paramKey = paramKey;
    target.lineIndex = lineIndex;
    target.rectMin = ImGui::GetItemRectMin();
    target.rectMax = ImGui::GetItemRectMax();
    m_dropTargets.push_back(std::move(target));
}

void NodeGraphEditor::HandleAssetDrop(const ImVec2& graphMin, const ImVec2& graphMax) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    if (peek && peek->IsDataType(kResourcePayload)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        for (const AssetDropTarget& target : m_dropTargets) {
            if (mouse.x < target.rectMin.x || mouse.y < target.rectMin.y ||
                mouse.x > target.rectMax.x || mouse.y > target.rectMax.y) {
                continue;
            }
            ImGui::GetForegroundDrawList()->AddRect(target.rectMin, target.rectMax,
                                                    IM_COL32(120, 205, 255, 255), 3.0f, 0, 2.0f);
            const ImGuiID id =
                ImGui::GetID(("##px-field-drop" + std::to_string(target.nodeId) + target.paramKey +
                              std::to_string(target.lineIndex))
                                 .c_str());
            if (ImGui::BeginDragDropTargetCustom(ImRect(target.rectMin, target.rectMax), id)) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kResourcePayload)) {
                    ApplyAssetToField(
                        target.nodeId, target.paramKey, target.lineIndex,
                        std::string(static_cast<const char*>(payload->Data),
                                    payload->DataSize > 0 ? payload->DataSize - 1 : 0));
                }
                ImGui::EndDragDropTarget();
            }
            return;  // a field consumed (or is hovering) the drag
        }
    }

    const ImRect graphRect(graphMin, graphMax);
    if (ImGui::BeginDragDropTargetCustom(graphRect, ImGui::GetID((Title() + "Drop").c_str()))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
            std::string asset(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
            m_createPosition = ImGui::GetMousePos();
            ApplyAssetToSelection(asset);
        }
        ImGui::EndDragDropTarget();
    }
}

void NodeGraphEditor::ApplyAssetToField(int nodeId, const std::string& paramKey, int lineIndex,
                                        const std::string& asset) {
    Node* node = FindNode(nodeId);
    if (!node || asset.empty()) {
        return;
    }
    Parameter* parameter = FindParameter(*node, paramKey);
    if (!parameter) {
        return;
    }
    if (lineIndex >= 0 && paramKey == "voices") {
        std::vector<std::string> voices = SplitTextLines(parameter->stringValue);
        if (Parameter* text = FindParameter(*node, "text")) {
            voices.resize(SplitTextLines(text->stringValue).size());
        }
        if (static_cast<std::size_t>(lineIndex) >= voices.size()) {
            return;
        }
        voices[static_cast<std::size_t>(lineIndex)] = asset;
        parameter->stringValue = JoinTextLines(voices);
    } else {
        parameter->stringValue = asset;
    }
    MarkDirty();
    Log("Set " + paramKey + " = " + asset);
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
            }
            else {
                ImGui::Dummy(ImVec2(0.0f, 22.0f));
            }
            ImGui::TableSetColumnIndex(1);
            if (row < node.outputs.size()) {
                RenderPin(node, node.outputs[row]);
            }
            else {
                ImGui::Dummy(ImVec2(0.0f, 22.0f));
            }
        }
        ImGui::EndTable();
    }

    if (node.type == "dialogue") {
        RenderDialogueTextEditor(node);
    }
    else {
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
                std::clamp((headerMax.x - headerMin.x) / (kHeaderTextureScale * static_cast<float>(m_headerTextureWidth)), 0.0f, 1.0f), std::clamp((headerMax.y - headerMin.y) / (kHeaderTextureScale * static_cast<float>(m_headerTextureHeight)), 0.0f, 1.0f)
            );
            ImColor headerTint = definition->accent;
            headerTint.Value.w = selected ? 0.90f : 0.84f;
            draw->AddImageRounded(m_headerTexture, headerMin, headerMax, ImVec2(0.0f, 0.0f), uv, headerTint, kNodeRounding, ImDrawFlags_RoundCornersTop);
            draw->AddRectFilled(headerMin, headerMax, selected ? IM_COL32(8, 12, 20, 56) : IM_COL32(8, 12, 20, 84), kNodeRounding, ImDrawFlags_RoundCornersTop);
        }
        else {
            ImColor fallback = definition->accent;
            fallback.Value.w = selected ? 0.50f : 0.38f;
            draw->AddRectFilled(headerMin, headerMax, fallback, kNodeRounding, ImDrawFlags_RoundCornersTop);
        }
    }

    draw->AddRect(rect.Min, rect.Max, nodeBorder, kNodeRounding, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.2f);

    // Debugger breakpoint marker (red dot hanging off the node's top-left corner).
    if (m_breakpointLines) {
        if (m_nodeCommandLine.empty() && m_kind == GraphKind::Scenario) {
            (void)CompileScenarioV4();  // populate the node→line map lazily
        }
        if (const std::set<int>* lines = m_breakpointLines()) {
            const int cmdLine = CommandLineForNode(node.id);
            if (cmdLine > 0 && lines->count(cmdLine) != 0) {
                const ImVec2 center(rect.Min.x + 2.0f, rect.Min.y + 2.0f);
                draw->AddCircleFilled(center, 7.0f, IM_COL32(232, 72, 60, 255));
                draw->AddCircle(center, 7.0f, IM_COL32(20, 12, 12, 255), 0, 1.5f);
            }
        }
    }
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
    }
    else {
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
            RenderParameter(*parameter, true, node.id);
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
    const auto& characters = ContextOptions("dialogue", "character");
    if (!characters.empty()) {
        const auto selected = std::find_if(characters.begin(), characters.end(),
            [&](const FieldOption& option) { return option.value == character->stringValue; });
        const std::string label = selected == characters.end()
            ? (character->stringValue.empty() ? "Select..." : character->stringValue)
            : selected->label;
        if (ImGui::Button((label + "  v##character").c_str(), {112.0f, 0.0f})) {
            m_inNodePopup = InNodePopup{node.id, "character", false, true};
        }
    } else {
        changed |= ImGui::InputText("##character", &character->stringValue);
    }
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("Speed");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(74.0f);
    changed |= ImGui::DragInt("##speed", &speed->intValue, 1.0f, static_cast<int>(speed->minValue), static_cast<int>(speed->maxValue));

    ImGui::TextDisabled("Color");
    ImGui::SameLine(0.0f, 8.0f);
    InNodeColorButton("##text-color", node.id, *color);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::TextDisabled("Outline");
    ImGui::SameLine(0.0f, 8.0f);
    InNodeColorButton("##outline-color", node.id, *outline);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::TextDisabled("Effect");
    ImGui::SameLine(0.0f, 8.0f);
    InNodeOptionButton(node.id, *effect, 112.0f);

    Parameter* voicesParam = FindParameter(node, "voices");
    std::vector<std::string> lines = SplitTextLines(text->stringValue);
    while (node.dialogueLineIds.size() < lines.size()) node.dialogueLineIds.push_back(Uuid::Random());
    if (node.dialogueLineIds.size() > lines.size()) node.dialogueLineIds.resize(lines.size());
    std::vector<std::string> voices =
        voicesParam ? SplitTextLines(voicesParam->stringValue) : std::vector<std::string>{};
    voices.resize(lines.size());

    int removeIndex = -1;
    int insertAfter = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%02d", static_cast<int>(i + 1));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::InputText("##line", &lines[i])) {
            changed = true;
        }
        if (voicesParam) {
            ImGui::SameLine(0.0f, 4.0f);
            const bool hasVoice = !voices[i].empty();
            ImGui::PushStyleColor(ImGuiCol_Text, hasVoice ? ImVec4(0.55f, 0.85f, 0.65f, 1.0f)
                                                          : ImVec4(0.45f, 0.49f, 0.55f, 1.0f));
            if (ImGui::SmallButton(hasVoice ? "V" : "v")) {
                m_inNodePopup =
                    InNodePopup{ node.id, "voices", false, true, static_cast<int>(i) };
            }
            ImGui::PopStyleColor();
            RecordDropTarget(node.id, "voices", static_cast<int>(i));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", hasVoice ? voices[i].c_str()
                                                 : "Voice: none (click or drop audio)");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kResourcePayload)) {
                    voices[i].assign(static_cast<const char*>(payload->Data),
                                     payload->DataSize > 0 ? payload->DataSize - 1 : 0);
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
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
        voices.insert(voices.begin() + insertAfter + 1, std::string{});
        node.dialogueLineIds.insert(node.dialogueLineIds.begin() + insertAfter + 1, Uuid::Random());
        changed = true;
    }
    if (removeIndex >= 0 && lines.size() > 1) {
        lines.erase(lines.begin() + removeIndex);
        voices.erase(voices.begin() + removeIndex);
        node.dialogueLineIds.erase(node.dialogueLineIds.begin() + removeIndex);
        changed = true;
    }
    if (changed) {
        text->stringValue = JoinTextLines(lines);
        if (voicesParam) {
            voicesParam->stringValue = JoinTextLines(voices);
        }
        MarkDirty();
    }
    ImGui::EndGroup();
}

void NodeGraphEditor::RenderUnboundParameters(Node& node) {
    std::vector<Parameter*> fields;
    for (Parameter& parameter : node.parameters) {
        const auto pinIt = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const Pin& pin) { return pin.parameterKey == parameter.key; });
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
        RenderParameter(*parameter, true, node.id);
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
            }
            else {
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

    bool deleted = false;
    if (ed::BeginDelete()) {
        ed::NodeId nodeId = 0;
        while (ed::QueryDeletedNode(&nodeId)) {
            if (ed::AcceptDeletedItem()) {
                RemoveNode(static_cast<int>(nodeId.Get()));
                deleted = true;
            }
        }
        ed::LinkId linkId = 0;
        while (ed::QueryDeletedLink(&linkId)) {
            if (ed::AcceptDeletedItem()) {
                RemoveLink(static_cast<int>(linkId.Get()));
                deleted = true;
            }
        }
    }
    ed::EndDelete();

    const ImGuiIO& io = ImGui::GetIO();
    const bool deletePressed = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (!deleted && deletePressed && !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        DeleteSelection();
    }
    if (io.KeyCtrl && !io.WantTextInput &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopySelection();
        } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            PasteClipboard(ed::ScreenToCanvas(ImGui::GetMousePos()));
        } else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            DuplicateSelection();
        }
    }
    // Ctrl+Z/Y are routed here by the application when this window is focused
    // (EditorApp::HandleShortcuts), so they are not handled again locally.
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
    ImGui::SeparatorText("Canvas");
    if (ImGui::MenuItem("Add Group")) {
        AddGroup(canvasPosition);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                        m_clipboard.is_object() && m_clipboard.contains("nodes"))) {
        PasteClipboard(canvasPosition);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NodeGraphEditor::RenderNodeContextMenu() {
    if (ImGui::BeginPopup("Node Menu")) {
        if (GroupNode* group = FindGroup(m_contextNodeId)) {
            ImGui::TextDisabled("Group");
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("##group-title", &group->title)) {
                MarkDirty();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Group")) {
                RemoveNode(m_contextNodeId);
            }
            ImGui::EndPopup();
            return;
        }
        if (const Node* node = FindNode(m_contextNodeId)) {
            if (const NodeTemplate* definition = FindTemplate(node->type)) {
                ImGui::TextUnformatted(definition->title.c_str());
                ImGui::TextDisabled("%s", definition->description.c_str());
                ImGui::Separator();
            }
        }
        if (m_toggleBreakpoint) {
            const int cmdLine = CommandLineForNode(m_contextNodeId);
            const bool hasBp = cmdLine > 0 && m_breakpointLines && m_breakpointLines() &&
                               m_breakpointLines()->count(cmdLine) != 0;
            ImGui::BeginDisabled(cmdLine <= 0);
            if (ImGui::MenuItem(hasBp ? "Remove Breakpoint" : "Set Breakpoint", nullptr, hasBp)) {
                m_toggleBreakpoint(cmdLine);
            }
            ImGui::EndDisabled();
            if (m_dirty) {
                ImGui::TextDisabled("(unsaved edits may shift lines — Save first)");
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            CopySelection();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            DuplicateSelection();
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
        }
        else {
            ImGui::SeparatorText("Fields");
            if (node->parameters.empty()) {
                ImGui::TextDisabled("This node has no configurable parameters yet.");
            }
            for (Parameter& parameter : node->parameters) {
                RenderParameter(parameter, false, node->id);
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

void NodeGraphEditor::RenderParameter(Parameter& parameter, bool compact, int nodeId) {
    ImGui::PushID(parameter.key.c_str());
    const std::string controlId = "##value";
    if (!compact) {
        ImGui::TextUnformatted(parameter.label.c_str());
    }
    if (compact) {
        ImGui::SetNextItemWidth(kCompactParameterWidth);
    }
    else {
        ImGui::SetNextItemWidth(-1.0f);
    }
    bool changed = false;
    Node* owner = FindNode(nodeId);
    std::string optionKey = parameter.key;
    if (owner && parameter.key == "expression") {
        if (const auto* character = FindParameter(*owner, "id"))
            optionKey += ":" + character->stringValue;
    }
    const auto& contextOptions = ContextOptions(owner ? owner->type : std::string_view{}, optionKey);
    const std::string searchKey = std::to_string(nodeId) + "/" + parameter.key;
    const auto optionLabel = [&]() -> std::string {
        const auto found = std::find_if(contextOptions.begin(), contextOptions.end(),
            [&](const auto& option) { return option.value == parameter.stringValue; });
        return found == contextOptions.end() ? parameter.stringValue : found->label;
    };
    const auto renderContextCombo = [&](const std::string& preview) -> bool {
        bool edited = false;
        if (ImGui::BeginCombo(controlId.c_str(), preview.c_str())) {
            auto& search = m_fieldOptionSearch[searchKey];
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##filter", "搜尋…", &search);
            const std::string needle = Lower(search);
            for (const auto& option : contextOptions) {
                if (!needle.empty() && Lower(option.label + " " + option.value + " " + option.detail).find(needle) == std::string::npos) continue;
                const bool selected = option.value == parameter.stringValue;
                if (ImGui::Selectable((option.label + "##" + option.value).c_str(), selected)) {
                    parameter.stringValue = option.value; edited = true;
                }
                if (!option.detail.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", option.detail.c_str());
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable("清除選擇")) { parameter.stringValue.clear(); edited = true; }
            ImGui::EndCombo();
        }
        return edited;
    };
    const auto renderContextButton = [&](const std::string& preview) {
        const std::string label = (preview.empty() ? "選擇…" : preview) + "  v";
        if (ImGui::Button((label + "##context-picker").c_str(),
                          {compact ? kCompactParameterWidth : -1.0f, 0.0f}))
            m_inNodePopup = InNodePopup{nodeId, parameter.key, false, true};
    };
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
            if (compact) {
                InNodeColorButton(controlId.c_str(), nodeId, parameter);
            }
            else {
                changed = ImGui::ColorEdit4(controlId.c_str(), &parameter.colorValue.x);
            }
            break;
        case ParamType::Option:
            if (parameter.key == "pos") {
                static const std::array<std::pair<const char*,const char*>,3> positions{{
                    {"左","1"},{"中","2"},{"右","3"}}};
                for(std::size_t index=0;index<positions.size();++index){
                    if(index)ImGui::SameLine(0.0f,3.0f);
                    const bool selected=parameter.stringValue==positions[index].second;
                    if(selected)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if(ImGui::Button(positions[index].first,{compact?46.0f:58.0f,0.0f})){
                        parameter.stringValue=positions[index].second;changed=true;
                    }
                    if(selected)ImGui::PopStyleColor();
                }
            }
            else if (compact) {
                InNodeOptionButton(nodeId, parameter, kCompactParameterWidth);
            }
            else if (ImGui::BeginCombo(controlId.c_str(), parameter.stringValue.c_str())) {
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
            if (parameter.type == ParamType::String && !parameter.multiline &&
                !contextOptions.empty()) {
                const std::string label = optionLabel();
                if (compact) renderContextButton(label);
                else changed = renderContextCombo(label.empty() ? "選擇…" : label);
            }
            else if (parameter.multiline && parameter.type == ParamType::String) {
                changed = ImGui::InputTextMultiline(controlId.c_str(), &parameter.stringValue, ImVec2(compact ? kCompactParameterWidth : -1.0f, compact ? 96.0f : 156.0f));
            }
            else if (parameter.type == ParamType::Asset && !contextOptions.empty()) {
                std::string label = optionLabel();
                if (label.empty()) label = "選擇素材…";
                if (compact) renderContextButton(label);
                else changed = renderContextCombo(label);
            }
            else {
                changed = ImGui::InputText(controlId.c_str(), &parameter.stringValue,
                    parameter.type == ParamType::Asset ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
            }
            if (parameter.type == ParamType::Asset) {
                // In-node fields never receive payload hover (the canvas eats
                // it); record the rect for the post-ed::End() hit test.
                RecordDropTarget(nodeId, parameter.key);
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
                }
                else if (!selected.empty() && ImGui::SmallButton("Use Selected Asset")) {
                    parameter.stringValue = selected;
                    changed = true;
                }
            }
            break;
    }
    if (changed && owner && (owner->type == "character" || owner->type == "char") &&
        parameter.key == "id") {
        if (auto* expression = FindParameter(*owner, "expression")) {
            const auto& expressions = ContextOptions(owner->type, "expression:" + parameter.stringValue);
            expression->stringValue = expressions.empty() ? std::string{} : expressions.front().value;
        }
    }
    if (changed) MarkDirty();
    ImGui::PopID();
}

void NodeGraphEditor::InNodeOptionButton(int nodeId, Parameter& parameter, float width) {
    ImGui::PushID(parameter.key.c_str());
    const std::string label = (parameter.stringValue.empty() ? "none" : parameter.stringValue) + "  v";
    if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f))) {
        m_inNodePopup = InNodePopup{ nodeId, parameter.key, false, true };
    }
    ImGui::PopID();
}

void NodeGraphEditor::InNodeColorButton(const char* id, int nodeId, Parameter& parameter) {
    if (ImGui::ColorButton(id, parameter.colorValue, ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip, ImVec2(36.0f, 0.0f))) {
        m_inNodePopup = InNodePopup{ nodeId, parameter.key, true, true };
    }
}

void NodeGraphEditor::RenderInNodePopups() {
    if (m_inNodePopup.requestOpen) {
        ImGui::OpenPopup("##in-node-popup");
        m_inNodePopup.requestOpen = false;
    }
    if (!ImGui::BeginPopup("##in-node-popup")) {
        return;
    }
    Node* node = FindNode(m_inNodePopup.nodeId);
    Parameter* parameter = node ? FindParameter(*node, m_inNodePopup.paramKey) : nullptr;
    if (!parameter) {
        ImGui::CloseCurrentPopup();
    }
    else if (m_inNodePopup.paramKey == "voices" && m_inNodePopup.lineIndex >= 0) {
        const std::size_t idx = static_cast<std::size_t>(m_inNodePopup.lineIndex);
        std::vector<std::string> voices = SplitTextLines(parameter->stringValue);
        if (Parameter* textParam = FindParameter(*node, "text")) {
            voices.resize(SplitTextLines(textParam->stringValue).size());
        }
        if (idx >= voices.size()) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled("Voice for line %d", m_inNodePopup.lineIndex + 1);
            const auto& voiceOptions = ContextOptions(node->type, "voice");
            const auto selectedOption = std::find_if(voiceOptions.begin(), voiceOptions.end(),
                [&](const FieldOption& option) { return option.value == voices[idx]; });
            const std::string preview = selectedOption != voiceOptions.end()
                ? selectedOption->label : (voices[idx].empty() ? "選擇語音…" : voices[idx]);
            ImGui::SetNextItemWidth(280.0f);
            bool edited = false;
            if (!voiceOptions.empty() && ImGui::BeginCombo("##voice-picker", preview.c_str())) {
                const std::string searchKey = std::to_string(node->id) + "/voices/" +
                                              std::to_string(idx);
                auto& search = m_fieldOptionSearch[searchKey];
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##voice-search", "搜尋語音…", &search);
                const std::string needle = Lower(search);
                for (const auto& option : voiceOptions) {
                    if (!needle.empty() &&
                        Lower(option.label + " " + option.value + " " + option.detail)
                                .find(needle) == std::string::npos)
                        continue;
                    if (ImGui::Selectable((option.label + "##" + option.value).c_str(),
                                          option.value == voices[idx])) {
                        voices[idx] = option.value;
                        edited = true;
                    }
                    if (!option.detail.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", option.detail.c_str());
                }
                ImGui::Separator();
                if (ImGui::Selectable("清除語音")) {
                    voices[idx].clear();
                    edited = true;
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(280.0f);
            edited |= ImGui::InputTextWithHint("##voiceline", "或輸入檔名／路徑",
                                               &voices[idx]);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kResourcePayload)) {
                    voices[idx].assign(static_cast<const char*>(payload->Data),
                                       payload->DataSize > 0 ? payload->DataSize - 1 : 0);
                    edited = true;
                }
                ImGui::EndDragDropTarget();
            }
            const std::string selected =
                m_selectedResource ? m_selectedResource() : std::string{};
            if (!selected.empty()) {
                if (ImGui::SmallButton(("Use: " + selected).c_str())) {
                    voices[idx] = selected;
                    edited = true;
                }
            }
            if (!voices[idx].empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    voices[idx].clear();
                    edited = true;
                }
            }
            ImGui::TextDisabled("Bare filenames resolve under Content/Audio/Voice/.");
            if (edited) {
                parameter->stringValue = JoinTextLines(voices);
                MarkDirty();
            }
        }
    }
    else if (m_inNodePopup.isColor) {
        if (ImGui::ColorPicker4("##picker", &parameter->colorValue.x, ImGuiColorEditFlags_AlphaBar)) {
            MarkDirty();
        }
    }
    else {
        std::string optionKey = parameter->key;
        if (parameter->key == "expression") {
            if (const auto* character = FindParameter(*node, "id"))
                optionKey += ":" + character->stringValue;
        }
        const auto& contextOptions = ContextOptions(node->type, optionKey);
        if (!contextOptions.empty()) {
            const std::string searchKey = std::to_string(node->id) + "/" + parameter->key;
            auto& search = m_fieldOptionSearch[searchKey];
            ImGui::SetNextItemWidth(280.0f);
            ImGui::InputTextWithHint("##context-search", "搜尋…", &search);
            const std::string needle = Lower(search);
            for (const auto& option : contextOptions) {
                if (!needle.empty() &&
                    Lower(option.label + " " + option.value + " " + option.detail)
                            .find(needle) == std::string::npos)
                    continue;
                const bool selected = option.value == parameter->stringValue;
                if (ImGui::Selectable((option.label + "##" + option.value).c_str(), selected)) {
                    parameter->stringValue = option.value;
                    if ((node->type == "character" || node->type == "char") &&
                        parameter->key == "id") {
                        if (auto* expression = FindParameter(*node, "expression")) {
                            const auto& expressions = ContextOptions(
                                node->type, "expression:" + parameter->stringValue);
                            expression->stringValue = expressions.empty()
                                ? std::string{} : expressions.front().value;
                        }
                    }
                    MarkDirty();
                    ImGui::CloseCurrentPopup();
                }
                if (!option.detail.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", option.detail.c_str());
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable("清除選擇")) {
                parameter->stringValue.clear();
                MarkDirty();
                ImGui::CloseCurrentPopup();
            }
        } else {
            for (const std::string& option : parameter->options) {
                const bool selected = option == parameter->stringValue;
                if (ImGui::Selectable(option.c_str(), selected)) {
                    parameter->stringValue = option;
                    MarkDirty();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
        }
    }
    ImGui::EndPopup();
}

void NodeGraphEditor::ApplyAssetToSelection(const std::string& runtimePath) {
    if (runtimePath.empty()) {
        return;
    }
    if (Lower(fs::path(runtimePath).extension().string()) == ".pxscenario") {
        OpenDocument(runtimePath);
        return;
    }
    if (Node* node = FindNode(m_selectedNodeId)) {
        // Only fill Asset-typed parameters: dropping a file must never clobber
        // plain string fields like a speaker name.
        for (Parameter& parameter : node->parameters) {
            if (parameter.type == ParamType::Asset) {
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

void NodeGraphEditor::CreateNodeForAsset(const std::string& runtimePath) {
    if(runtimePath.empty())return;const std::string extension=Lower(fs::path(runtimePath).extension().string());if(extension==".pxscenario"){OpenDocument(runtimePath);return;}std::string type="asset";const std::string lower=Lower(runtimePath);
    if(extension==".png"||extension==".jpg"||extension==".jpeg"||extension==".webp"||extension==".bmp")type=lower.find("character")!=std::string::npos?"character":"background";
    else if(extension==".mp3"||extension==".ogg"||extension==".wav"||extension==".flac"||extension==".opus")type=lower.find("voice")!=std::string::npos?"voice":lower.find("sfx")!=std::string::npos?"se":"bgm";
    else if(extension==".mp4"||extension==".webm"||extension==".mkv"||extension==".mov"||extension==".mpg"||extension==".mpeg")type="video";else if(extension==".pxscene")type="spawn_ui";else if(extension==".pxanim")type="animation";
    const int previous=m_selectedNodeId;Node* created=AddNode(type,ed::ScreenToCanvas(m_createPosition));if(!created)return;
    if(type=="spawn_ui"&&m_project){for(const auto& route:m_project->manifest.routes)if(fs::path(route.scene).lexically_normal()==fs::path(runtimePath).lexically_normal())if(auto* field=FindParameter(*created,"ui")){field->stringValue=route.id;break;}}
    else for(Parameter& parameter:created->parameters)if(parameter.type==ParamType::Asset){parameter.stringValue=runtimePath;break;}const int createdId=created->id;
    if(Node* from=FindNode(previous);from&&!from->outputs.empty())if(Node* to=FindNode(createdId);to&&!to->inputs.empty())AddLink(from->outputs.front().id,to->inputs.front().id);m_selectedNodeId=createdId;Log("Created "+type+" node from asset: "+runtimePath);
}

void NodeGraphEditor::AlignSelection(const bool horizontal) {
    std::vector<ed::NodeId> selected(static_cast<std::size_t>(std::max(0, ed::GetSelectedObjectCount())));
    const int count = ed::GetSelectedNodes(selected.data(), static_cast<int>(selected.size()));
    if (count < 2) return;
    float coordinate = 0.0f;
    for (int index = 0; index < count; ++index) {
        const ImVec2 position = ed::GetNodePosition(selected[static_cast<std::size_t>(index)]);
        coordinate += horizontal ? position.y : position.x;
    }
    coordinate /= static_cast<float>(count);
    for (int index = 0; index < count; ++index) {
        const auto id = selected[static_cast<std::size_t>(index)];
        ImVec2 position = ed::GetNodePosition(id);
        if (horizontal) position.y = coordinate; else position.x = coordinate;
        ed::SetNodePosition(id, position);
        if (Node* node = FindNode(static_cast<int>(id.Get()))) node->position = position;
    }
    MarkDirty();
}

void NodeGraphEditor::RenderMiniMap() const {
    if (m_nodes.empty()) return;
    ImVec2 minimum = m_nodes.front().position, maximum = m_nodes.front().position;
    for (const auto& node : m_nodes) {
        minimum.x = std::min(minimum.x, node.position.x); minimum.y = std::min(minimum.y, node.position.y);
        maximum.x = std::max(maximum.x, node.position.x + 540.0f); maximum.y = std::max(maximum.y, node.position.y + 220.0f);
    }
    const ImVec2 size(184.0f, 116.0f);
    const ImVec2 windowPosition = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const ImVec2 origin(windowPosition.x + windowSize.x - size.x - 18.0f,
                        windowPosition.y + windowSize.y - size.y - 18.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, IM_COL32(8, 11, 18, 218), 7.0f);
    draw->AddRect(origin, origin + size, IM_COL32(100, 125, 165, 190), 7.0f);
    const float scale = std::min((size.x - 12.0f) / std::max(1.0f, maximum.x - minimum.x),
                                 (size.y - 12.0f) / std::max(1.0f, maximum.y - minimum.y));
    for (const auto& node : m_nodes) {
        const ImVec2 point(origin.x + 6.0f + (node.position.x - minimum.x) * scale,
                           origin.y + 6.0f + (node.position.y - minimum.y) * scale);
        const ImVec2 extent(std::max(3.0f, 540.0f * scale), std::max(2.0f, 180.0f * scale));
        draw->AddRectFilled(point, point + extent,
            node.id == m_selectedNodeId ? IM_COL32(255, 204, 100, 235) : IM_COL32(105, 170, 235, 185), 1.5f);
    }
    draw->AddText(origin + ImVec2(7.0f, 5.0f), IM_COL32(210, 220, 238, 210), "MINIMAP");
}

const std::vector<NodeGraphEditor::FieldOption>& NodeGraphEditor::ContextOptions(
    const std::string_view nodeType, const std::string_view parameterKey) const {
    static const std::vector<FieldOption> empty;
    if (!m_fieldOptions || nodeType.empty()) return empty;
    const std::string cacheKey = std::string(nodeType) + "\n" + std::string(parameterKey);
    m_fieldOptionsCache[cacheKey] = m_fieldOptions(nodeType, parameterKey);
    if ((nodeType == "jump" || nodeType == "call") && parameterKey == "target") {
        auto& options = m_fieldOptionsCache[cacheKey];
        for (const auto& node : m_nodes) {
            if (node.type == "scenario_start") continue;
            const auto* definition = FindTemplate(node.type);
            std::string label = definition ? definition->title : node.type;
            if (const auto* title = FindParameter(node, "title"); title && !title->stringValue.empty())
                label += " · " + title->stringValue;
            options.push_back({"@" + node.stableId.ToString(), std::move(label),
                               "本 Scenario 的穩定節點目標"});
        }
    }
    return m_fieldOptionsCache[cacheKey];
}

void NodeGraphEditor::FrameSelection() {
    EnsureContext();
    ed::SetCurrentEditor(m_context);
    ed::NavigateToContent();
}

bool NodeGraphEditor::FocusStatement(const std::string_view statementId) {
    const auto parsed = Uuid::Parse(statementId);
    if (!parsed) return false;
    const auto found = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const Node& node) {
        return node.stableId == *parsed ||
               std::find(node.dialogueLineIds.begin(), node.dialogueLineIds.end(), *parsed) !=
                   node.dialogueLineIds.end();
    });
    if (found == m_nodes.end()) return false;
    m_focusNodeId = found->id;
    m_selectedNodeId = found->id;
    return true;
}

std::vector<ExportArtifact> NodeGraphEditor::BuildArtifacts() const {auto layoutPath=fs::path(CurrentRuntimePath());layoutPath.replace_extension(".pxlayout");vn::scenario::ScenarioLayoutDocument layout;layout.scenario=m_editDocumentId;for(const auto& node:m_nodes)if(node.type!="scenario_start")layout.nodes.push_back({node.type=="dialogue"&&!node.dialogueLineIds.empty()?node.dialogueLineIds.front():node.stableId,{node.position.x,node.position.y},{},{}});return {ExportArtifact{Title(),fs::path(CurrentRuntimePath()),Compile()},ExportArtifact{"Scenario Layout",layoutPath,vn::scenario::WriteScenarioLayout(layout)}};}

std::string NodeGraphEditor::Compile() const { return CompileScenarioV4(); }

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
        for (Pin& pin : node.inputs)
            if (pin.id == id) return &pin;
        for (Pin& pin : node.outputs)
            if (pin.id == id) return &pin;
    }
    return nullptr;
}

const NodeGraphEditor::Pin* NodeGraphEditor::FindPin(int id) const {
    for (const Node& node : m_nodes) {
        for (const Pin& pin : node.inputs)
            if (pin.id == id) return &pin;
        for (const Pin& pin : node.outputs)
            if (pin.id == id) return &pin;
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
    return std::any_of(m_links.begin(), m_links.end(), [&](const Link& link) { return link.startPinId == id || link.endPinId == id; });
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
        case PinType::Flow:
            return ImColor(245, 248, 255);
        case PinType::Bool:
            return ImColor(255, 104, 104);
        case PinType::Int:
            return ImColor(92, 220, 170);
        case PinType::Float:
            return ImColor(126, 232, 106);
        case PinType::String:
            return ImColor(175, 118, 255);
        case PinType::Asset:
            return ImColor(80, 194, 255);
        case PinType::Object:
            return ImColor(228, 188, 104);
    }
    return ImColor(255, 255, 255);
}

const char* NodeGraphEditor::PinTypeName(PinType type) {
    switch (type) {
        case PinType::Flow:
            return "Flow";
        case PinType::Bool:
            return "Bool";
        case PinType::Int:
            return "Int";
        case PinType::Float:
            return "Float";
        case PinType::String:
            return "String";
        case PinType::Asset:
            return "Asset";
        case PinType::Object:
            return "Object";
    }
    return "Unknown";
}

std::string NodeGraphEditor::CompileScenarioV4() const {
    vn::scenario::ScenarioDocument document;
    document.id=m_editDocumentId.Empty()?Uuid::FromName(CurrentRuntimePath()):m_editDocumentId;
    document.name=fs::path(CurrentRuntimePath()).stem().string();
    const Node* start=nullptr;std::unordered_map<int,Uuid> ids;std::unordered_map<int,Uuid> tails;
    for(const Node& node:m_nodes){if(node.type=="scenario_start"){start=&node;if(const auto* title=FindParameter(node,"title");title&&!title->stringValue.empty())document.name=title->stringValue;continue;}Uuid first=node.stableId.Empty()?Uuid::FromName(document.id.ToString()+"/node/"+std::to_string(node.id)):node.stableId;if(node.type=="dialogue"&&!node.dialogueLineIds.empty())first=node.dialogueLineIds.front();ids[node.id]=first;tails[node.id]=node.type=="dialogue"&&!node.dialogueLineIds.empty()?node.dialogueLineIds.back():first;}
    const auto commandFor=[](const std::string& type){if(type=="dialogue")return std::string("say");if(type=="background"||type=="transition")return std::string("bg");if(type=="character")return std::string("char");if(type=="variable")return std::string("var");if(type=="animate_actor")return std::string("anim");if(type=="stop_bgm")return std::string("stopbgm");if(type=="spawn_ui")return std::string("route");return type;};
    const auto scalar=[](const std::string& value)->Variant{if(value=="true")return true;if(value=="false")return false;try{std::size_t used=0;const auto integer=std::stoll(value,&used);if(used==value.size())return static_cast<std::int64_t>(integer);}catch(...){}try{std::size_t used=0;const auto number=std::stod(value,&used);if(used==value.size())return number;}catch(...){}return value;};
    m_nodeCommandLine.clear();
    for(const Node& node:m_nodes){if(node.type=="scenario_start")continue;
        if(node.type=="dialogue"){
            const auto* text=FindParameter(node,"text");const auto* voices=FindParameter(node,"voices");const auto lines=SplitTextLines(text?text->stringValue:std::string{});const auto voiceLines=SplitTextLines(voices?voices->stringValue:std::string{});Uuid previous{};m_nodeCommandLine[node.id]=static_cast<int>(document.nodes.size()+1);
            for(std::size_t index=0;index<lines.size();++index){const Uuid lineId=index<node.dialogueLineIds.size()?node.dialogueLineIds[index]:Uuid::FromName((node.stableId.Empty()?document.id:node.stableId).ToString()+"/line/"+std::to_string(index));vn::scenario::ScenarioNode line{lineId,"say",{}};line.parameters["textId"]=lineId.ToString();line.parameters["blockId"]=(node.stableId.Empty()?ids.at(node.id):node.stableId).ToString();line.parameters["value"]=lines[index];if(const auto* speaker=FindParameter(node,"speaker");speaker&&!speaker->stringValue.empty())line.parameters["speaker"]=speaker->stringValue;if(const auto* character=FindParameter(node,"character");character&&!character->stringValue.empty())line.parameters["char"]=character->stringValue;if(const auto* speed=FindParameter(node,"speed"))line.parameters["speed"]=static_cast<std::int64_t>(speed->intValue);if(const auto* effect=FindParameter(node,"effect");effect&&!effect->stringValue.empty())line.parameters["effect"]=effect->stringValue;if(const auto* color=FindParameter(node,"color"))line.parameters["color"]=ColorToCsv(color->colorValue);if(const auto* outline=FindParameter(node,"outline"))line.parameters["outline"]=ColorToCsv(outline->colorValue);if(index<voiceLines.size()&&!voiceLines[index].empty()){const auto resolved=m_resourceResolver?m_resourceResolver(voiceLines[index]):std::nullopt;line.parameters["voice"]=resolved?Variant(*resolved):Variant(ResourceRefValue{{},voiceLines[index]});}document.nodes.push_back(std::move(line));if(!previous.Empty())document.edges.push_back({Uuid::FromName(document.id.ToString()+"/dialogue/"+previous.ToString()+"/"+lineId.ToString()),previous,"flow",lineId,"in"});previous=lineId;}
            if(!previous.Empty())tails[node.id]=previous;continue;
        }
        vn::scenario::ScenarioNode output;output.id=ids.at(node.id);output.command=commandFor(node.type);const auto* descriptor=vn::CommandRegistry::Global().Find(output.command);
        for(const Parameter& parameter:node.parameters){std::string name=parameter.key;if(node.type=="branch"&&(name=="lhs"||name=="operator"||name=="rhs"))continue;if(node.type=="dialogue"&&name=="text")name="value";if(node.type=="dialogue"&&(name=="voices"||name=="character"))continue;if(node.type=="variable"&&name=="var")name="name";if(node.type=="variable"&&name=="op")continue;if(node.type=="spawn_ui"&&name=="ui")name="route";const vn::CommandParameterDescriptor* schema=nullptr;if(descriptor){const auto found=std::find_if(descriptor->parameters.begin(),descriptor->parameters.end(),[&name](const auto& candidate){return candidate.name==name;});if(found!=descriptor->parameters.end())schema=&*found;}Variant value;if(parameter.type==ParamType::Bool)value=parameter.boolValue;else if(parameter.type==ParamType::Int)value=static_cast<std::int64_t>(parameter.intValue);else if(parameter.type==ParamType::Float)value=static_cast<double>(parameter.floatValue);else if(parameter.type==ParamType::Color)value=ColorToCsv(parameter.colorValue);else if(schema&&schema->type==VariantType::ResourceRef){if(parameter.stringValue.empty())continue;const auto resolved=m_resourceResolver?m_resourceResolver(parameter.stringValue):std::nullopt;value=resolved?Variant(*resolved):Variant(ResourceRefValue{{},parameter.stringValue});}else if(schema&&schema->type==VariantType::Integer){try{value=static_cast<std::int64_t>(std::stoll(parameter.stringValue));}catch(...){value=std::int64_t{0};}}else if(schema&&schema->type==VariantType::Number){try{value=std::stod(parameter.stringValue);}catch(...){value=0.0;}}else if(schema&&schema->type==VariantType::Bool)value=parameter.stringValue=="true";else if(schema&&schema->widget==vn::CommandEditorWidget::Expression)value=vn::ExpressionToValue(vn::Expression::Literal(scalar(parameter.stringValue)));else value=parameter.stringValue;output.parameters[name]=std::move(value);}
        if(node.type=="dialogue"){output.parameters["textId"]=output.id.ToString();if(const auto* voices=FindParameter(node,"voices");voices&&!voices->stringValue.empty()){const auto first=voices->stringValue.substr(0,voices->stringValue.find('\n'));const auto resolved=m_resourceResolver?m_resourceResolver(first):std::nullopt;output.parameters["voice"]=resolved?Variant(*resolved):Variant(ResourceRefValue{{},first});}}
        if(node.type=="choice")output.parameters["textId"]=output.id.ToString();if(node.type=="variable"){const auto* operation=FindParameter(node,"op");const auto* raw=FindParameter(node,"value");if(raw){if(operation&&operation->stringValue=="add"){const Variant parsed=scalar(raw->stringValue);if(const auto* integer=parsed.TryGet<std::int64_t>())output.parameters["add"]=static_cast<double>(*integer);else if(const auto* number=parsed.TryGet<double>())output.parameters["add"]=*number;}else output.parameters["value"]=scalar(raw->stringValue);}}
        if(node.type=="branch"){const auto* lhs=FindParameter(node,"lhs");const auto* op=FindParameter(node,"operator");const auto* rhs=FindParameter(node,"rhs");vn::ExpressionOperator expressionOperator=vn::ExpressionOperator::Equal;if(op){if(op->stringValue=="!=")expressionOperator=vn::ExpressionOperator::NotEqual;else if(op->stringValue=="<")expressionOperator=vn::ExpressionOperator::Less;else if(op->stringValue=="<=")expressionOperator=vn::ExpressionOperator::LessEqual;else if(op->stringValue==">")expressionOperator=vn::ExpressionOperator::Greater;else if(op->stringValue==">=")expressionOperator=vn::ExpressionOperator::GreaterEqual;}output.parameters["expression"]=vn::ExpressionToValue(vn::Expression::Binary(expressionOperator,vn::Expression::Variable(lhs?lhs->stringValue:std::string{}),vn::Expression::Literal(scalar(rhs?rhs->stringValue:std::string{}))));}
        m_nodeCommandLine[node.id]=static_cast<int>(document.nodes.size()+1);document.nodes.push_back(std::move(output));}
    if(document.nodes.empty())return {};document.entry=document.nodes.front().id;
    for(const Link& link:m_links){const Pin* from=FindPin(link.startPinId);const Pin* to=FindPin(link.endPinId);if(!from||!to||from->type!=PinType::Flow||to->type!=PinType::Flow)continue;if(start&&from->nodeId==start->id){if(ids.contains(to->nodeId))document.entry=ids.at(to->nodeId);continue;}if(!ids.contains(from->nodeId)||!ids.contains(to->nodeId))continue;std::string port="flow";if(const Node* owner=FindNode(from->nodeId);owner){const auto output=std::find_if(owner->outputs.begin(),owner->outputs.end(),[from](const Pin& pin){return pin.id==from->id;});if(output!=owner->outputs.end()){if(owner->type=="choice"&&output!=owner->outputs.begin())port="choice";else if(owner->type=="branch")port=Lower(output->label);}}document.edges.push_back({link.stableId.Empty()?Uuid::FromName(document.id.ToString()+"/edge/"+std::to_string(link.id)):link.stableId,tails.at(from->nodeId),port,ids.at(to->nodeId),"in"});}
    const auto validation=vn::scenario::ValidateScenario(document,vn::CommandRegistry::Global(),CurrentRuntimePath());for(const auto& diagnostic:validation.diagnostics)diag::Emit(diagnostic);if(!validation.Valid())return {};return vn::scenario::WriteScenario(document);
}

std::string NodeGraphEditor::NormalizeScenarioRuntimePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = Trim(value);
    if (value.empty()) {
        return {};
    }
    fs::path path(value);if(path.is_absolute())return {};for(const auto& part:path)if(part=="..")return {};
    if(!value.starts_with("Content/Scenario/"))value="Content/Scenario/"+fs::path(value).filename().generic_string();
    fs::path normalized(value);normalized.replace_extension(".pxscenario");return normalized.lexically_normal().generic_string();
}

std::string NodeGraphEditor::Trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

std::string NodeGraphEditor::Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
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

}  // namespace px::editor
