#define IMGUI_DEFINE_MATH_OPERATORS

#include "BlueprintEditor.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <widgets.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ed = ax::NodeEditor;
using ax::Drawing::IconType;
namespace fs = std::filesystem;

namespace PrismatiX::Editor {

namespace {

constexpr float kNodeRounding = 12.0f;
constexpr float kNodeContentWidth = 540.0f;
constexpr float kInputColumnWidth = 318.0f;
constexpr float kOutputColumnWidth = 182.0f;
constexpr float kOutputAlignWidth = 158.0f;
constexpr float kCompactParameterWidth = 176.0f;
constexpr float kPinIconSize = 18.0f;
constexpr float kPinLabelSpacing = 6.0f;
constexpr float kHeaderTextureScale = 6.0f;
constexpr const char* kGeneratedSceneScriptPath = "Script/chapter1.pds";

struct ParsedPdsCommand {
    std::string name;
    std::unordered_map<std::string, std::string> arguments;
};

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string TrimCopy(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string UnescapePdsValue(std::string_view value) {
    std::string text = TrimCopy(value);
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
    }

    std::string unescaped;
    unescaped.reserve(text.size());
    bool escaping = false;
    for (char ch : text) {
        if (escaping) {
            switch (ch) {
                case 'n':
                    unescaped.push_back('\n');
                    break;
                case 't':
                    unescaped.push_back('\t');
                    break;
                default:
                    unescaped.push_back(ch);
                    break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        unescaped.push_back(ch);
    }
    if (escaping) {
        unescaped.push_back('\\');
    }
    return unescaped;
}

std::optional<ParsedPdsCommand> ParsePdsCommand(std::string_view rawLine) {
    const std::string line = TrimCopy(rawLine);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return std::nullopt;
    }

    ParsedPdsCommand command;
    const std::string body = line.substr(1, line.size() - 2);
    size_t cursor = 0;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
        ++cursor;
    }
    const size_t nameStart = cursor;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) == 0) {
        ++cursor;
    }
    command.name = body.substr(nameStart, cursor - nameStart);

    while (cursor < body.size()) {
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= body.size()) {
            break;
        }

        const size_t keyStart = cursor;
        while (cursor < body.size() && body[cursor] != '=' && std::isspace(static_cast<unsigned char>(body[cursor])) == 0) {
            ++cursor;
        }
        std::string key = body.substr(keyStart, cursor - keyStart);
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= body.size() || body[cursor] != '=') {
            command.arguments[std::move(key)] = {};
            continue;
        }

        ++cursor;
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= body.size()) {
            command.arguments[std::move(key)] = {};
            break;
        }

        std::string value;
        if (body[cursor] == '"' || body[cursor] == '\'') {
            const char quote = body[cursor++];
            while (cursor < body.size()) {
                const char ch = body[cursor++];
                if (ch == '\\' && cursor < body.size()) {
                    value.push_back('\\');
                    value.push_back(body[cursor++]);
                    continue;
                }
                if (ch == quote) {
                    break;
                }
                value.push_back(ch);
            }
            command.arguments[std::move(key)] = UnescapePdsValue(std::string(1, quote) + value + std::string(1, quote));
            continue;
        }

        const size_t valueStart = cursor;
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) == 0) {
            ++cursor;
        }
        command.arguments[std::move(key)] = UnescapePdsValue(body.substr(valueStart, cursor - valueStart));
    }

    return command;
}

std::string ExtractLuaArgs(std::string_view rawLine) {
    const std::string line = TrimCopy(rawLine);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return {};
    }

    const std::string body = line.substr(1, line.size() - 2);
    size_t cursor = 0;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
        ++cursor;
    }
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) == 0) {
        ++cursor;
    }

    std::string args;
    bool firstToken = true;
    while (cursor < body.size()) {
        while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= body.size()) {
            break;
        }

        const size_t tokenStart = cursor;
        bool inQuotes = false;
        bool escaping = false;
        char quote = '\0';
        while (cursor < body.size()) {
            const char ch = body[cursor];
            if (escaping) {
                escaping = false;
                ++cursor;
                continue;
            }
            if (inQuotes) {
                if (ch == '\\') {
                    escaping = true;
                } else if (ch == quote) {
                    inQuotes = false;
                }
                ++cursor;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inQuotes = true;
                quote = ch;
                ++cursor;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                break;
            }
            ++cursor;
        }

        const std::string token = body.substr(tokenStart, cursor - tokenStart);
        if (ToLowerCopy(token).rfind("fn=", 0) == 0) {
            continue;
        }
        if (!firstToken) {
            args += ' ';
        }
        args += token;
        firstToken = false;
    }

    return args;
}

ImVec2 SceneNodePosition(size_t index) {
    const float x = -760.0f + static_cast<float>(index % 4) * 330.0f;
    const float y = -240.0f + static_cast<float>(index / 4) * 220.0f;
    return ImVec2(x, y);
}

}  // namespace

BlueprintEditor::BlueprintEditor(BlueprintFlavor flavor, LogCallback logCallback) : m_flavor(flavor), m_logCallback(std::move(logCallback)) {
    BuildLibrary();
    SeedDefaults();
}

BlueprintEditor::~BlueprintEditor() {
    if (m_context && ImGui::GetCurrentContext()) {
        ed::DestroyEditor(m_context);
    }
    m_context = nullptr;
}

void BlueprintEditor::ResetToDefaults() {
    m_nodes.clear();
    m_links.clear();
    m_sceneDocuments.clear();
    m_activeSceneDocumentIndex = 0;
    m_sceneDocumentsNeedRefresh = (m_flavor == BlueprintFlavor::SceneScript);
    m_selectedNodeId = 0;
    m_selectedLinkId = 0;
    m_contextNodeId = 0;
    m_contextLinkId = 0;
    m_pendingLinkPinId = 0;
    m_createPopupPosition = ImVec2(0.0f, 0.0f);
    m_createPopupOpen = false;
    m_nextId = 1;
    m_navigateCountdown = 3;
    SeedDefaults();
}

void BlueprintEditor::EnsureEditorContext() {
    if (m_context || !ImGui::GetCurrentContext()) {
        return;
    }

    ed::Config config;
    config.SettingsFile = nullptr;
    m_context = ed::CreateEditor(&config);
    m_navigateCountdown = std::max(m_navigateCountdown, 3);
}

void BlueprintEditor::SetHeaderTexture(ImTextureID texture, int width, int height) {
    m_headerTexture = texture;
    m_headerTextureWidth = width;
    m_headerTextureHeight = height;
}

void BlueprintEditor::SetSelectedResourceCallback(SelectedResourceCallback callback) {
    m_selectedResourceCallback = std::move(callback);
}

void BlueprintEditor::SetProjectRoot(const fs::path& projectRoot) {
    if (m_projectRoot == projectRoot) {
        return;
    }
    m_projectRoot = projectRoot;
    if (m_flavor == BlueprintFlavor::SceneScript) {
        m_sceneDocumentsNeedRefresh = true;
        RefreshSceneDocuments(true);
    }
}

void BlueprintEditor::BuildLibrary() {
    auto makeString = [&](std::string key, std::string label, std::string value) {
        NodeParameter parameter;
        parameter.key = std::move(key);
        parameter.label = std::move(label);
        parameter.kind = ParameterKind::String;
        parameter.stringValue = std::move(value);
        return parameter;
    };

    auto makeAsset = [&](std::string key, std::string label, std::string value) {
        NodeParameter parameter = makeString(std::move(key), std::move(label), std::move(value));
        parameter.kind = ParameterKind::Asset;
        return parameter;
    };

    auto makeOption = [&](std::string key, std::string label, std::string value, std::vector<std::string> options) {
        NodeParameter parameter = makeString(std::move(key), std::move(label), std::move(value));
        parameter.kind = ParameterKind::Option;
        parameter.options = std::move(options);
        return parameter;
    };

    auto makeInt = [&](std::string key, std::string label, int value, int minValue, int maxValue, float speed = 1.0f) {
        NodeParameter parameter;
        parameter.key = std::move(key);
        parameter.label = std::move(label);
        parameter.kind = ParameterKind::Integer;
        parameter.intValue = value;
        parameter.minValue = static_cast<float>(minValue);
        parameter.maxValue = static_cast<float>(maxValue);
        parameter.speed = speed;
        return parameter;
    };

    auto makeFloat = [&](std::string key, std::string label, float value, float minValue, float maxValue, float speed = 0.1f) {
        NodeParameter parameter;
        parameter.key = std::move(key);
        parameter.label = std::move(label);
        parameter.kind = ParameterKind::Float;
        parameter.floatValue = value;
        parameter.minValue = minValue;
        parameter.maxValue = maxValue;
        parameter.speed = speed;
        return parameter;
    };

    auto makeBool = [&](std::string key, std::string label, bool value) {
        NodeParameter parameter;
        parameter.key = std::move(key);
        parameter.label = std::move(label);
        parameter.kind = ParameterKind::Boolean;
        parameter.boolValue = value;
        return parameter;
    };

    auto inFlow = [&](std::string name) { return PinTemplate{ std::move(name), PinDataType::Flow, ed::PinKind::Input, {} }; };

    auto outFlow = [&](std::string name) { return PinTemplate{ std::move(name), PinDataType::Flow, ed::PinKind::Output, {} }; };

    auto inString = [&](std::string name, std::string key) { return PinTemplate{ std::move(name), PinDataType::String, ed::PinKind::Input, std::move(key) }; };

    auto inAsset = [&](std::string name, std::string key) { return PinTemplate{ std::move(name), PinDataType::Asset, ed::PinKind::Input, std::move(key) }; };

    auto inFloat = [&](std::string name, std::string key) { return PinTemplate{ std::move(name), PinDataType::Float, ed::PinKind::Input, std::move(key) }; };

    auto inBool = [&](std::string name, std::string key) { return PinTemplate{ std::move(name), PinDataType::Bool, ed::PinKind::Input, std::move(key) }; };

    auto outString = [&](std::string name, std::string key = {}) { return PinTemplate{ std::move(name), PinDataType::String, ed::PinKind::Output, std::move(key) }; };

    auto outAsset = [&](std::string name, std::string key = {}) { return PinTemplate{ std::move(name), PinDataType::Asset, ed::PinKind::Output, std::move(key) }; };

    auto outFloat = [&](std::string name, std::string key = {}) { return PinTemplate{ std::move(name), PinDataType::Float, ed::PinKind::Output, std::move(key) }; };

    auto outBool = [&](std::string name, std::string key = {}) { return PinTemplate{ std::move(name), PinDataType::Bool, ed::PinKind::Output, std::move(key) }; };

    if (m_flavor == BlueprintFlavor::Entrypoint) {
        m_library.push_back(
            NodeTemplate{ "boot_runtime",
                          "Boot Runtime",
                          "Boot",
                          "Defines the startup title and typography used by the generated game loop.",
                          ImColor(80, 184, 255),
                          {},
                          { outFlow("Exec") },
                          { makeString("game_title", "Game Title", "PrismatiX Narrative"), makeString("font_name", "Font Name", "NotoSansTC-Bold.ttf"), makeInt("font_size", "Font Size", 32, 14, 96) } }
        );

        m_library.push_back(
            NodeTemplate{ "configure_systems",
                          "Configure Systems",
                          "Boot",
                          "Builds the Scene, Transition, Notification and FX managers that feed the Lua entrypoint.",
                          ImColor(72, 218, 182),
                          { inFlow("In") },
                          { outFlow("Out") },
                          { makeOption("transition_style", "Transition Style", "dissolve", { "fade", "wipe", "dissolve" }), makeInt("transition_speed", "Transition Speed", 10, 1, 40), makeInt("notification_size", "Notification Size", 20, 12, 48) } }
        );

        m_library.push_back(
            NodeTemplate{ "run_splashes",
                          "Run Splash Scripts",
                          "Boot",
                          "Plays engine and game splash Lua scripts before handing control to the first scene.",
                          ImColor(255, 184, 92),
                          { inFlow("In") },
                          { outFlow("Out") },
                          { makeAsset("engine_splash", "Engine Splash", "Scripts/engine_splash.lua"), makeAsset("game_splash", "Game Splash", "Scripts/splash.lua") } }
        );

        m_library.push_back(
            NodeTemplate{ "preload_scene",
                          "Preload Scene Module",
                          "Boot",
                          "Preloads one Lua scene module into memory so scene switches stay smooth.",
                          ImColor(255, 132, 132),
                          { inFlow("In"), inAsset("Scene Module", "scene_module") },
                          { outFlow("Out") },
                          { makeAsset("scene_module", "Scene Module", "Scripts/scenes/title_scene.lua") } }
        );

        m_library.push_back(
            NodeTemplate{ "switch_scene",
                          "Switch Scene",
                          "Boot",
                          "Creates the first scene instance and exposes the .pds script path the play scene should load.",
                          ImColor(198, 127, 255),
                          { inFlow("In"), inAsset("Scene Module", "scene_module"), inString("Ctor", "scene_ctor"), inAsset("Scene Script", "scene_script") },
                          { outFlow("Out") },
                          { makeAsset("scene_module", "Scene Module", "Scripts/scenes/title_scene.lua"), makeString("scene_ctor", "Scene Constructor", "TitleScene"), makeAsset("scene_script", "Scene Script", kGeneratedSceneScriptPath) } }
        );

        m_library.push_back(NodeTemplate{ "game_loop", "Game Loop", "Loop", "Begins the generated while Engine.IsRunning() loop and collects all downstream frame operations.", ImColor(118, 234, 141), { inFlow("In") }, { outFlow("Loop") }, {} });

        m_library.push_back(NodeTemplate{ "update_fx", "Update FX", "Loop", "Updates screen effects and forwards camera shake offsets to the renderer.", ImColor(93, 170, 255), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(NodeTemplate{ "update_notification", "Update Notifications", "Loop", "Ticks notification toasts and lightweight HUD overlays.", ImColor(93, 170, 255), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(NodeTemplate{ "update_transition", "Update Transition", "Loop", "Advances the active transition using the current viewport width.", ImColor(93, 170, 255), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(NodeTemplate{ "update_scene", "Update Scene", "Loop", "Feeds mouse input into the scene graph and lets the active scene react.", ImColor(93, 170, 255), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(
            NodeTemplate{ "clear_screen",
                          "Clear Screen",
                          "Loop",
                          "Sets the renderer background before the frame content is drawn.",
                          ImColor(54, 133, 230),
                          { inFlow("In") },
                          { outFlow("Out") },
                          { makeInt("red", "Red", 0, 0, 255), makeInt("green", "Green", 0, 0, 255), makeInt("blue", "Blue", 0, 0, 255), makeInt("alpha", "Alpha", 255, 0, 255) } }
        );

        m_library.push_back(NodeTemplate{ "render_scene", "Render Scene", "Loop", "Draws the current scene with the active logical viewport dimensions.", ImColor(54, 133, 230), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(NodeTemplate{ "render_notification", "Render Notifications", "Loop", "Renders notification stacks after the scene content.", ImColor(54, 133, 230), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(NodeTemplate{ "render_transition", "Render Transition", "Loop", "Draws fullscreen transition coverage as the last overlay before present.", ImColor(54, 133, 230), { inFlow("In") }, { outFlow("Out") }, {} });

        m_library.push_back(NodeTemplate{ "present_frame", "Present Frame", "Loop", "Swaps the SDL renderer buffers and completes the frame.", ImColor(54, 133, 230), { inFlow("In") }, { outFlow("Out") }, {} });
    }
    else {
        m_library.push_back(
            NodeTemplate{
                "scene_entry", "Scene Entry", "Timeline", "Entry for a real Data/Script/*.pds document.", ImColor(80, 184, 255), {}, { outFlow("Begin") }, { makeString("scene_name", "Scene Name", "chapter1"), makeString("heading", "Heading", "第一章"), makeAsset("background", "Background", "") } }
        );

        m_library.push_back(
            NodeTemplate{ "set_background", "Set Background", "Timeline", "Set a background.", ImColor(59, 174, 255), { inFlow("In"), inAsset("Asset", "background") }, { outFlow("Out") }, { makeAsset("background", "Background", "title_bg.jpg") } }
        );

        m_library.push_back(
            NodeTemplate{ "play_bgm",
                          "Play BGM",
                          "Timeline",
                          "Play a repeating background music.",
                          ImColor(93, 208, 162),
                          { inFlow("In"), inAsset("Track", "bgm"), inFloat("Volume", "volume") },
                          { outFlow("Out") },
                          { makeAsset("bgm", "BGM Asset", "bgm_theme.mp3"), makeFloat("volume", "Volume", 0.85f, 0.0f, 1.0f, 0.01f) } }
        );

        m_library.push_back(
            NodeTemplate{ "show_dialogue",
                          "Show Dialogue",
                          "Timeline",
                          "Emit a [text] command and the following dialogue body.",
                          ImColor(255, 186, 92),
                          { inFlow("In"), inString("Speaker", "speaker"), inString("Text", "text") },
                          { outFlow("Out") },
                          { makeString("speaker", "Speaker", "Aster"), makeString("text", "Dialogue Text", "The node editor now drives the scene script preview.") } }
        );

        m_library.push_back(
            NodeTemplate{ "transition",
                          "Transition",
                          "Timeline",
                          "Queue a transition.",
                          ImColor(198, 127, 255),
                          { inFlow("In"), inString("Style", "style"), inFloat("Speed", "speed") },
                          { outFlow("Out") },
                          { makeOption("style", "Style", "dissolve", { "fade", "wipe", "dissolve" }), makeFloat("speed", "Speed", 10.0f, 1.0f, 40.0f, 0.25f) } }
        );

        m_library.push_back(
            NodeTemplate{ "animate_actor",
                          "Animate Actor",
                          "Timeline",
                          "Portrait animation.",
                          ImColor(255, 132, 132),
                          { inFlow("In"), inString("Target", "target"), inFloat("Duration", "duration"), inFloat("Move X", "move_x") },
                          { outFlow("Out") },
                          { makeString("target", "Target", "heroine_portrait"), makeFloat("duration", "Duration", 0.45f, 0.05f, 5.0f, 0.01f), makeFloat("move_x", "Move X", 120.0f, -800.0f, 800.0f, 1.0f) } }
        );

        m_library.push_back(
            NodeTemplate{ "spawn_ui",
                          "Spawn UI",
                          "Timeline",
                          "idk if ill need this or not",
                          ImColor(93, 170, 255),
                          { inFlow("In"), inAsset("UI Script", "ui_script") },
                          { outFlow("Out") },
                          { makeAsset("ui_script", "UI Script", "Scripts/components/ui_components.lua") } }
        );

        m_library.push_back(
            NodeTemplate{ "present_choice",
                          "Present Choice",
                          "Timeline",
                          "Emit one or more [choice] lines.",
                          ImColor(255, 184, 92),
                          { inFlow("In"), inString("Option A", "option_a"), inString("A Target", "option_a_target"), inString("Option B", "option_b"), inString("B Target", "option_b_target"), inString("Option C", "option_c"), inString("C Target", "option_c_target"), inString("Option D", "option_d"), inString("D Target", "option_d_target") },
                          { outFlow("Out") },
                          { makeString("option_a", "Option A", "Start"), makeString("option_a_target", "Option A Target", ""), makeString("option_b", "Option B", "Leave"), makeString("option_b_target", "Option B Target", ""), makeString("option_c", "Option C", ""), makeString("option_c_target", "Option C Target", ""), makeString("option_d", "Option D", ""), makeString("option_d_target", "Option D Target", "") } }
        );

        m_library.push_back(NodeTemplate{ "wait_input", "Wait Input", "Timeline", "Wait.", ImColor(109, 124, 146), { inFlow("In"), inBool("Auto Advance", "auto_advance") }, { outFlow("Out") }, { makeBool("auto_advance", "Auto Advance", false) } });

        m_library.push_back(
            NodeTemplate{
                "jump_scene", "Jump", "Timeline", "Jump to a label or another .pds script.", ImColor(118, 234, 141), { inFlow("In"), inString("Target", "target_scene") }, { outFlow("Out") }, { makeString("target_scene", "Target", "chapter2.pds") } }
        );

        m_library.push_back(
            NodeTemplate{ "scene_label",
                          "Label",
                          "Timeline",
                          "Emit a *label marker.",
                          ImColor(142, 153, 255),
                          { inFlow("In"), inString("Label", "label") },
                          { outFlow("Out") },
                          { makeString("label", "Label", "start") } }
        );

        m_library.push_back(
            NodeTemplate{ "character_state",
                          "Character",
                          "Timeline",
                          "Emit a [char] command.",
                          ImColor(255, 132, 132),
                          { inFlow("In"), inString("Id", "char_id"), inString("Expression", "expression"), inString("Pos", "position") },
                          { outFlow("Out") },
                          { makeString("char_id", "Id", "girl"), makeString("expression", "Expression", "d"), makeString("position", "Pos", "2") } }
        );

        m_library.push_back(
            NodeTemplate{ "set_variable",
                          "Variable",
                          "Timeline",
                          "Emit a [var] command.",
                          ImColor(109, 198, 164),
                          { inFlow("In"), inString("Var", "var_name"), inString("Op", "op"), inString("Value", "value") },
                          { outFlow("Out") },
                          { makeString("var_name", "Var", "affection"), makeOption("op", "Op", "set", { "set", "add", "sub", "mul", "div" }), makeString("value", "Value", "0") } }
        );

        m_library.push_back(
            NodeTemplate{ "run_lua",
                          "Lua Command",
                          "Timeline",
                          "Emit a [lua] command with free-form arguments.",
                          ImColor(198, 127, 255),
                          { inFlow("In"), inString("Function", "fn"), inString("Args", "args") },
                          { outFlow("Out") },
                          { makeString("fn", "Function", "FX_Shake"), makeString("args", "Args", "duration=24 amplitude=18 frequency=1.0 decay=1.8") } }
        );

        m_library.push_back(
            NodeTemplate{ "stop_bgm",
                          "Stop BGM",
                          "Timeline",
                          "Emit [stopbgm].",
                          ImColor(160, 160, 170),
                          { inFlow("In") },
                          { outFlow("Out") },
                          {} }
        );

        m_library.push_back(
            NodeTemplate{ "raw_command",
                          "Raw Command",
                          "Timeline",
                          "Fallback node for unsupported commands so existing scripts still round-trip.",
                          ImColor(124, 124, 138),
                          { inFlow("In"), inString("Command", "command") },
                          { outFlow("Out") },
                          { makeString("command", "Command", "[raw]") } }
        );
    }

    m_library.push_back(
        NodeTemplate{ "string_literal", "String Literal", "Literals", "Reusable text value that can drive string pins across the graph.", ImColor(149, 108, 255), {}, { outString("Value", "value") }, { makeString("value", "Value", "Literal text") } }
    );

    m_library.push_back(
        NodeTemplate{ "asset_reference",
                      "Asset Reference",
                      "Literals",
                      "Reusable asset path node for backgrounds, scripts, fonts and audio references.",
                      ImColor(84, 189, 255),
                      {},
                      { outAsset("Asset", "path") },
                      { makeAsset("path", "Asset Path", "Scripts/scenes/title_scene.lua") } }
    );

    m_library.push_back(
        NodeTemplate{
            "float_literal", "Float Literal", "Literals", "Reusable numeric node for speeds, volumes and animation timings.", ImColor(118, 234, 141), {}, { outFloat("Value", "value") }, { makeFloat("value", "Value", 1.0f, -999.0f, 999.0f, 0.01f) } }
    );

    m_library.push_back(NodeTemplate{ "bool_literal", "Bool Literal", "Literals", "Reusable true/false source node for quick preview toggles.", ImColor(255, 110, 110), {}, { outBool("Value", "value") }, { makeBool("value", "Value", true) } });
}

void BlueprintEditor::SeedDefaults() {
    if (m_flavor == BlueprintFlavor::Entrypoint) {
        SeedEntrypointGraph();
    }
    else {
        SeedSceneGraph();
    }
}

void BlueprintEditor::SeedEntrypointGraph() {
    m_nodes.reserve(16);

    AddNode("boot_runtime", ImVec2(-920.0f, -80.0f));
    AddNode("configure_systems", ImVec2(-650.0f, -80.0f));
    AddNode("run_splashes", ImVec2(-380.0f, -80.0f));
    AddNode("preload_scene", ImVec2(-110.0f, -200.0f));
    AddNode("preload_scene", ImVec2(-110.0f, 40.0f));
    AddNode("switch_scene", ImVec2(180.0f, -80.0f));
    AddNode("game_loop", ImVec2(500.0f, -80.0f));
    AddNode("update_fx", ImVec2(790.0f, -320.0f));
    AddNode("update_notification", ImVec2(1070.0f, -320.0f));
    AddNode("update_transition", ImVec2(1370.0f, -320.0f));
    AddNode("update_scene", ImVec2(1650.0f, -320.0f));
    AddNode("clear_screen", ImVec2(790.0f, 30.0f));
    AddNode("render_scene", ImVec2(1070.0f, 30.0f));
    AddNode("render_notification", ImVec2(1370.0f, 30.0f));
    AddNode("render_transition", ImVec2(1670.0f, 30.0f));
    AddNode("present_frame", ImVec2(1960.0f, 30.0f));

    if (auto* preloadTitle = FindNode(m_nodes[3].id)) {
        if (auto* module = FindParameter(*preloadTitle, "scene_module")) {
            module->stringValue = "Scripts/scenes/title_scene.lua";
        }
    }

    if (auto* preloadPlay = FindNode(m_nodes[4].id)) {
        if (auto* module = FindParameter(*preloadPlay, "scene_module")) {
            module->stringValue = "Scripts/scenes/play_scene.lua";
        }
    }

    AddLinkByNodeIndex(0, 0, 1, 0);
    AddLinkByNodeIndex(1, 0, 2, 0);
    AddLinkByNodeIndex(2, 0, 3, 0);
    AddLinkByNodeIndex(3, 0, 4, 0);
    AddLinkByNodeIndex(4, 0, 5, 0);
    AddLinkByNodeIndex(5, 0, 6, 0);
    AddLinkByNodeIndex(6, 0, 7, 0);
    AddLinkByNodeIndex(7, 0, 8, 0);
    AddLinkByNodeIndex(8, 0, 9, 0);
    AddLinkByNodeIndex(9, 0, 10, 0);
    AddLinkByNodeIndex(10, 0, 11, 0);
    AddLinkByNodeIndex(11, 0, 12, 0);
    AddLinkByNodeIndex(12, 0, 13, 0);
    AddLinkByNodeIndex(13, 0, 14, 0);
    AddLinkByNodeIndex(14, 0, 15, 0);
}

void BlueprintEditor::SeedSceneGraph() {
    m_nodes.reserve(12);

    AddNode("scene_entry", ImVec2(-760.0f, -80.0f));
    AddNode("set_background", ImVec2(-460.0f, -80.0f));
    AddNode("play_bgm", ImVec2(-140.0f, -250.0f));
    AddNode("show_dialogue", ImVec2(170.0f, -250.0f));
    AddNode("transition", ImVec2(510.0f, -250.0f));
    AddNode("animate_actor", ImVec2(850.0f, -250.0f));
    AddNode("spawn_ui", ImVec2(1170.0f, -250.0f));
    AddNode("present_choice", ImVec2(170.0f, 70.0f));
    AddNode("wait_input", ImVec2(510.0f, 70.0f));
    AddNode("jump_scene", ImVec2(850.0f, 70.0f));

    AddLinkByNodeIndex(0, 0, 1, 0);
    AddLinkByNodeIndex(1, 0, 2, 0);
    AddLinkByNodeIndex(2, 0, 3, 0);
    AddLinkByNodeIndex(3, 0, 4, 0);
    AddLinkByNodeIndex(4, 0, 5, 0);
    AddLinkByNodeIndex(5, 0, 6, 0);
    AddLinkByNodeIndex(6, 0, 7, 0);
    AddLinkByNodeIndex(7, 0, 8, 0);
    AddLinkByNodeIndex(8, 0, 9, 0);
}

BlueprintEditor::NodeInstance* BlueprintEditor::AddNode(const std::string& typeId, const ImVec2& position) {
    const NodeTemplate* definition = FindTemplate(typeId);
    if (!definition) {
        return nullptr;
    }

    NodeInstance node;
    node.id = ed::NodeId(m_nextId++);
    node.templateRef = definition;
    node.parameters = definition->defaults;
    node.initialPosition = position;
    node.positionInitialized = false;

    for (const auto& input : definition->inputs) {
        node.inputs.push_back(PinInstance{ ed::PinId(m_nextId++), static_cast<int>(node.id.Get()), input.name, input.type, input.kind, input.parameterKey });
    }

    for (const auto& output : definition->outputs) {
        node.outputs.push_back(PinInstance{ ed::PinId(m_nextId++), static_cast<int>(node.id.Get()), output.name, output.type, output.kind, output.parameterKey });
    }

    m_nodes.push_back(std::move(node));
    NodeInstance* created = &m_nodes.back();
    if (m_context) {
        ed::SetCurrentEditor(m_context);
        ed::SetNodePosition(created->id, position);
        created->positionInitialized = true;
    }
    return created;
}

void BlueprintEditor::AddLink(ed::PinId from, ed::PinId to) {
    const PinInstance* source = FindPin(from);
    if (!source) {
        return;
    }

    m_links.push_back(LinkInstance{ ed::LinkId(m_nextId++), from, to, GetPinColor(source->type) });
}

void BlueprintEditor::AddLinkByNodeIndex(size_t fromNodeIndex, size_t fromOutputIndex, size_t toNodeIndex, size_t toInputIndex) {
    if (fromNodeIndex >= m_nodes.size() || toNodeIndex >= m_nodes.size()) {
        return;
    }

    const auto& fromNode = m_nodes[fromNodeIndex];
    const auto& toNode = m_nodes[toNodeIndex];
    if (fromOutputIndex >= fromNode.outputs.size() || toInputIndex >= toNode.inputs.size()) {
        return;
    }

    AddLink(fromNode.outputs[fromOutputIndex].id, toNode.inputs[toInputIndex].id);
}

void BlueprintEditor::EnsureSceneDocuments() {
    if (m_flavor != BlueprintFlavor::SceneScript) {
        return;
    }
    if (m_sceneDocumentsNeedRefresh || (m_sceneDocuments.empty() && !m_projectRoot.empty())) {
        RefreshSceneDocuments(true);
    }
}

void BlueprintEditor::RefreshSceneDocuments(bool force) {
    if (m_flavor != BlueprintFlavor::SceneScript) {
        return;
    }
    if (!force && !m_sceneDocumentsNeedRefresh) {
        return;
    }

    CaptureActiveSceneDocument();
    std::unordered_map<std::string, SceneDocument> previousDocuments;
    for (const SceneDocument& document : m_sceneDocuments) {
        previousDocuments.emplace(document.path.string(), document);
    }

    const std::string previousPath = CurrentDocumentPath().string();
    m_sceneDocuments.clear();
    m_activeSceneDocumentIndex = static_cast<size_t>(-1);

    for (const fs::path& path : EnumerateSceneDocuments()) {
        SceneDocument document;
        document.path = path;
        document.runtimePath = fs::relative(path, DataRoot()).generic_string();
        document.displayName = path.filename().string();

        auto previous = previousDocuments.find(path.string());
        if (previous != previousDocuments.end() && previous->second.dirty) {
            document.source = previous->second.source;
            document.dirty = true;
        } else {
            std::ifstream in(path, std::ios::binary);
            if (in.is_open()) {
                document.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
        }

        m_sceneDocuments.push_back(std::move(document));
    }

    if (m_sceneDocuments.empty()) {
        m_activeSceneDocumentIndex = 0;
        m_nodes.clear();
        m_links.clear();
        m_nextId = 1;
        SeedSceneGraph();
        m_sceneDocumentsNeedRefresh = false;
        return;
    }

    size_t nextIndex = 0;
    if (!previousPath.empty()) {
        for (size_t index = 0; index < m_sceneDocuments.size(); ++index) {
            if (m_sceneDocuments[index].path.string() == previousPath) {
                nextIndex = index;
                break;
            }
        }
    } else if (m_activeSceneDocumentIndex < m_sceneDocuments.size()) {
        nextIndex = m_activeSceneDocumentIndex;
    }

    m_sceneDocumentsNeedRefresh = false;
    LoadSceneDocument(nextIndex);
}

void BlueprintEditor::RenderSceneWorkspace() {
    EnsureSceneDocuments();
    RenderToolbar();
    ImGui::Separator();

    if (ImGui::BeginTable("scene-script-layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Story Files", ImGuiTableColumnFlags_WidthFixed, 280.0f);
        ImGui::TableSetupColumn("Node Graph", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        RenderSceneDocumentList();

        ImGui::TableSetColumnIndex(1);
        RenderGraph();

        ImGui::EndTable();
    }
}

void BlueprintEditor::RenderSceneDocumentList() {
    ImGui::BeginChild("scene-script-documents", ImVec2(0.0f, 0.0f), false);
    if (m_sceneDocuments.empty()) {
        ImGui::TextDisabled("No .pds files under Data/Script.");
        ImGui::TextDisabled("This graph editor edits Data/Script/chapter1.pds style story files.");
        ImGui::EndChild();
        return;
    }

    for (size_t index = 0; index < m_sceneDocuments.size(); ++index) {
        const SceneDocument& document = m_sceneDocuments[index];
        std::string label = document.displayName;
        if (document.dirty) {
            label += " *";
        }
        if (ImGui::Selectable(label.c_str(), index == m_activeSceneDocumentIndex)) {
            LoadSceneDocument(index);
        }
    }
    ImGui::EndChild();
}

void BlueprintEditor::CaptureActiveSceneDocument() {
    if (m_flavor != BlueprintFlavor::SceneScript) {
        return;
    }
    SceneDocument* document = ActiveSceneDocument();
    if (!document) {
        return;
    }
    const std::string content = EmitScenePds();
    if (content != document->source) {
        document->source = content;
        document->dirty = true;
    }
}

void BlueprintEditor::LoadSceneDocument(size_t index) {
    if (m_flavor != BlueprintFlavor::SceneScript || index >= m_sceneDocuments.size()) {
        return;
    }

    if (!m_sceneDocuments.empty() && m_activeSceneDocumentIndex < m_sceneDocuments.size()) {
        CaptureActiveSceneDocument();
    }

    m_activeSceneDocumentIndex = index;
    const SceneDocument& document = m_sceneDocuments[m_activeSceneDocumentIndex];
    LoadSceneGraph(document.source, fs::path(document.displayName).stem().string());
}

void BlueprintEditor::LoadSceneGraph(std::string_view source, std::string_view sceneName) {
    m_nodes.clear();
    m_links.clear();
    m_selectedNodeId = 0;
    m_selectedLinkId = 0;
    m_contextNodeId = 0;
    m_contextLinkId = 0;
    m_pendingLinkPinId = 0;
    m_createPopupPosition = ImVec2(0.0f, 0.0f);
    m_createPopupOpen = false;
    m_nextId = 1;
    m_navigateCountdown = 3;

    auto setParameter = [&](NodeInstance* node, std::string_view key, const std::string& value) {
        if (!node) {
            return;
        }
        if (NodeParameter* parameter = FindParameter(*node, key)) {
            if (parameter->kind == ParameterKind::Float) {
                try {
                    parameter->floatValue = std::stof(value);
                } catch (...) {
                    parameter->floatValue = 0.0f;
                }
            } else if (parameter->kind == ParameterKind::Integer) {
                try {
                    parameter->intValue = std::stoi(value);
                } catch (...) {
                    parameter->intValue = 0;
                }
            } else if (parameter->kind == ParameterKind::Boolean) {
                const std::string lowered = ToLowerCopy(value);
                parameter->boolValue = lowered == "true" || lowered == "1" || lowered == "yes";
            } else {
                parameter->stringValue = value;
            }
        }
    };

    size_t visualIndex = 0;
    auto appendNode = [&](const std::string& typeId) {
        return AddNode(typeId, SceneNodePosition(visualIndex++));
    };

    NodeInstance* entry = appendNode("scene_entry");
    setParameter(entry, "scene_name", std::string(sceneName));

    NodeInstance* previous = entry;
    std::istringstream stream{std::string(source)};
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    bool capturedHeading = false;
    for (size_t index = 0; index < lines.size(); ++index) {
        const std::string trimmed = TrimCopy(lines[index]);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed.front() == '#') {
            if (!capturedHeading) {
                setParameter(entry, "heading", TrimCopy(trimmed.substr(1)));
                capturedHeading = true;
            }
            continue;
        }

        NodeInstance* node = nullptr;
        if (trimmed.front() == '*') {
            node = appendNode("scene_label");
            setParameter(node, "label", trimmed.substr(1));
        } else if (auto command = ParsePdsCommand(trimmed)) {
            const std::string name = ToLowerCopy(command->name);
            if (name == "chapter") {
                if (const auto it = command->arguments.find("title"); it != command->arguments.end()) {
                    setParameter(entry, "heading", it->second);
                }
                if (const auto it = command->arguments.find("name"); it != command->arguments.end()) {
                    setParameter(entry, "scene_name", it->second);
                }
                if (const auto it = command->arguments.find("id"); it != command->arguments.end()) {
                    setParameter(entry, "scene_name", it->second);
                }
                continue;
            }

            if (name == "bg") {
                node = appendNode("set_background");
                if (const auto it = command->arguments.find("file"); it != command->arguments.end()) {
                    setParameter(node, "background", it->second);
                }
            } else if (name == "bgm") {
                node = appendNode("play_bgm");
                if (const auto it = command->arguments.find("file"); it != command->arguments.end()) {
                    setParameter(node, "bgm", it->second);
                }
                if (const auto it = command->arguments.find("volume"); it != command->arguments.end()) {
                    setParameter(node, "volume", it->second);
                }
            } else if (name == "text") {
                node = appendNode("show_dialogue");
                if (const auto it = command->arguments.find("speaker"); it != command->arguments.end()) {
                    setParameter(node, "speaker", it->second);
                }
                if (const auto it = command->arguments.find("content"); it != command->arguments.end()) {
                    setParameter(node, "text", it->second);
                } else {
                    std::vector<std::string> bodyLines;
                    size_t lookahead = index + 1;
                    while (lookahead < lines.size()) {
                        const std::string nextLine = TrimCopy(lines[lookahead]);
                        if (nextLine.empty()) {
                            if (!bodyLines.empty()) {
                                ++lookahead;
                                break;
                            }
                            ++lookahead;
                            continue;
                        }
                        if (nextLine.front() == '[' || nextLine.front() == '*' || nextLine.front() == '#') {
                            break;
                        }
                        bodyLines.push_back(lines[lookahead]);
                        ++lookahead;
                    }

                    std::ostringstream body;
                    for (size_t bodyIndex = 0; bodyIndex < bodyLines.size(); ++bodyIndex) {
                        if (bodyIndex > 0) {
                            body << '\n';
                        }
                        body << bodyLines[bodyIndex];
                    }
                    setParameter(node, "text", body.str());
                    if (!bodyLines.empty()) {
                        index = lookahead - 1;
                    }
                }
            } else if (name == "choice") {
                node = appendNode("present_choice");
                size_t choiceSlot = 0;
                size_t lookahead = index;
                while (lookahead < lines.size()) {
                    const auto nextChoice = ParsePdsCommand(TrimCopy(lines[lookahead]));
                    if (!nextChoice || ToLowerCopy(nextChoice->name) != "choice" || choiceSlot >= 4) {
                        break;
                    }
                    const char suffix = static_cast<char>('a' + choiceSlot);
                    if (const auto textIt = nextChoice->arguments.find("text"); textIt != nextChoice->arguments.end()) {
                        setParameter(node, std::string("option_") + suffix, textIt->second);
                    }
                    if (const auto targetIt = nextChoice->arguments.find("target"); targetIt != nextChoice->arguments.end()) {
                        setParameter(node, std::string("option_") + suffix + "_target", targetIt->second);
                    }
                    ++choiceSlot;
                    ++lookahead;
                }
                index = lookahead - 1;
            } else if (name == "jump") {
                node = appendNode("jump_scene");
                if (const auto it = command->arguments.find("target"); it != command->arguments.end()) {
                    setParameter(node, "target_scene", it->second);
                }
            } else if (name == "wait") {
                node = appendNode("wait_input");
                if (const auto it = command->arguments.find("auto_advance"); it != command->arguments.end()) {
                    setParameter(node, "auto_advance", it->second);
                }
            } else if (name == "char") {
                node = appendNode("character_state");
                if (const auto it = command->arguments.find("id"); it != command->arguments.end()) {
                    setParameter(node, "char_id", it->second);
                }
                if (const auto it = command->arguments.find("expression"); it != command->arguments.end()) {
                    setParameter(node, "expression", it->second);
                }
                if (const auto it = command->arguments.find("pos"); it != command->arguments.end()) {
                    setParameter(node, "position", it->second);
                }
            } else if (name == "var") {
                node = appendNode("set_variable");
                if (const auto it = command->arguments.find("var"); it != command->arguments.end()) {
                    setParameter(node, "var_name", it->second);
                }
                if (const auto it = command->arguments.find("op"); it != command->arguments.end()) {
                    setParameter(node, "op", it->second);
                }
                if (const auto it = command->arguments.find("val"); it != command->arguments.end()) {
                    setParameter(node, "value", it->second);
                }
            } else if (name == "lua") {
                node = appendNode("run_lua");
                if (const auto it = command->arguments.find("fn"); it != command->arguments.end()) {
                    setParameter(node, "fn", it->second);
                }
                setParameter(node, "args", ExtractLuaArgs(trimmed));
            } else if (name == "stopbgm") {
                node = appendNode("stop_bgm");
            } else {
                node = appendNode("raw_command");
                setParameter(node, "command", trimmed);
            }
        } else {
            node = appendNode("show_dialogue");
            setParameter(node, "speaker", "");
            setParameter(node, "text", lines[index]);
        }

        if (node && previous && !previous->outputs.empty() && !node->inputs.empty()) {
            AddLink(previous->outputs.front().id, node->inputs.front().id);
            previous = node;
        }
    }
}

void BlueprintEditor::SaveSceneDocument(size_t index) {
    if (m_flavor != BlueprintFlavor::SceneScript || index >= m_sceneDocuments.size()) {
        return;
    }
    if (index == m_activeSceneDocumentIndex) {
        CaptureActiveSceneDocument();
    }

    SceneDocument& document = m_sceneDocuments[index];
    std::ofstream out(document.path, std::ios::binary);
    if (!out.is_open()) {
        Log("Failed to save " + document.path.string());
        return;
    }
    out << document.source;
    document.dirty = false;
    Log("Saved " + document.runtimePath);
}

void BlueprintEditor::SaveAllSceneDocuments() {
    if (m_flavor != BlueprintFlavor::SceneScript) {
        return;
    }
    CaptureActiveSceneDocument();
    for (size_t index = 0; index < m_sceneDocuments.size(); ++index) {
        if (m_sceneDocuments[index].dirty) {
            SaveSceneDocument(index);
        }
    }
}

void BlueprintEditor::MarkSceneDocumentDirty() {
    if (m_flavor != BlueprintFlavor::SceneScript) {
        return;
    }
    if (SceneDocument* document = ActiveSceneDocument()) {
        document->source = EmitScenePds();
        document->dirty = true;
    }
}

const BlueprintEditor::SceneDocument* BlueprintEditor::ActiveSceneDocument() const {
    if (m_flavor != BlueprintFlavor::SceneScript || m_sceneDocuments.empty() || m_activeSceneDocumentIndex >= m_sceneDocuments.size()) {
        return nullptr;
    }
    return &m_sceneDocuments[m_activeSceneDocumentIndex];
}

BlueprintEditor::SceneDocument* BlueprintEditor::ActiveSceneDocument() {
    if (m_flavor != BlueprintFlavor::SceneScript || m_sceneDocuments.empty() || m_activeSceneDocumentIndex >= m_sceneDocuments.size()) {
        return nullptr;
    }
    return &m_sceneDocuments[m_activeSceneDocumentIndex];
}

fs::path BlueprintEditor::DataRoot() const {
    return m_projectRoot.empty() ? fs::path{} : (m_projectRoot / "Data");
}

fs::path BlueprintEditor::SceneScriptRoot() const {
    const fs::path dataRoot = DataRoot();
    return dataRoot.empty() ? fs::path{} : (dataRoot / "Script");
}

std::vector<fs::path> BlueprintEditor::EnumerateSceneDocuments() const {
    std::vector<fs::path> documents;
    const fs::path root = SceneScriptRoot();
    if (root.empty() || !fs::exists(root)) {
        return documents;
    }

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && ToLowerCopy(entry.path().extension().string()) == ".pds") {
            documents.push_back(entry.path());
        }
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

void BlueprintEditor::Render() {
    EnsureEditorContext();
    if (!m_context) {
        ImGui::TextDisabled("Node editor context is not ready yet.");
        return;
    }

    if (m_flavor == BlueprintFlavor::SceneScript) {
        RenderSceneWorkspace();
        return;
    }

    RenderToolbar();
    ImGui::Separator();
    RenderGraph();
}

void BlueprintEditor::RenderToolbar() {
    if (ImGui::Button("Frame Graph")) {
        NavigateToContent();
    }

    ImGui::SameLine();
    if (ImGui::Button("Add Node")) {
        m_pendingLinkPinId = 0;
        m_createPopupPosition = ImGui::GetMousePos();
        m_createPopupOpen = true;
        ImGui::OpenPopup("Create Blueprint Node");
    }

    if (m_flavor == BlueprintFlavor::SceneScript) {
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            RefreshSceneDocuments(true);
        }

        ImGui::SameLine();
        const bool hasActiveDocument = ActiveSceneDocument() != nullptr;
        const bool activeDirty = hasActiveDocument && ActiveSceneDocument()->dirty;
        if (!activeDirty) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save")) {
            SaveSceneDocument(m_activeSceneDocumentIndex);
        }
        if (!activeDirty) {
            ImGui::EndDisabled();
        }

        const bool hasDirtyDocument = std::any_of(m_sceneDocuments.begin(), m_sceneDocuments.end(), [](const SceneDocument& document) { return document.dirty; });
        ImGui::SameLine();
        if (!hasDirtyDocument) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save All")) {
            SaveAllSceneDocuments();
        }
        if (!hasDirtyDocument) {
            ImGui::EndDisabled();
        }
    }

    ImGui::SameLine();
    if (m_flavor == BlueprintFlavor::SceneScript) {
        if (const SceneDocument* document = ActiveSceneDocument()) {
            ImGui::TextDisabled("%s", document->runtimePath.c_str());
        } else {
            ImGui::TextDisabled("No story .pds selected");
        }
    } else {
        ImGui::TextDisabled("%zu nodes / %zu links", m_nodes.size(), m_links.size());
    }
}

void BlueprintEditor::RenderGraph() {
    ed::SetCurrentEditor(m_context);

    ImGui::BeginChild(m_flavor == BlueprintFlavor::Entrypoint ? "entrypoint-graph" : "scene-graph", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoMove);
    ed::Begin(m_flavor == BlueprintFlavor::Entrypoint ? "EntrypointGraph" : "SceneGraph");

    bool seededPositionsApplied = false;
    for (auto& node : m_nodes) {
        if (!node.positionInitialized) {
            ed::SetNodePosition(node.id, node.initialPosition);
            node.positionInitialized = true;
            seededPositionsApplied = true;
        }
    }
    if (seededPositionsApplied) {
        m_navigateCountdown = std::max(m_navigateCountdown, 2);
    }

    for (auto& node : m_nodes) {
        RenderNode(node);
    }

    for (const auto& link : m_links) {
        ed::Link(link.id, link.startPinId, link.endPinId, link.color, 2.5f);
    }

    if (!m_createPopupOpen) {
        HandleCreateDeleteInteractions();
    }
    RefreshSelectionState();

    if (m_navigateCountdown > 0) {
        --m_navigateCountdown;
        if (m_navigateCountdown == 0) {
            ed::NavigateToContent();
        }
    }

    ed::Suspend();
    if (ed::ShowNodeContextMenu(&m_contextNodeId)) {
        ImGui::OpenPopup("Node Context Menu");
    }
    else if (ed::ShowLinkContextMenu(&m_contextLinkId)) {
        ImGui::OpenPopup("Link Context Menu");
    }
    else if (ed::ShowBackgroundContextMenu()) {
        m_pendingLinkPinId = 0;
        m_createPopupPosition = ImGui::GetMousePos();
        m_createPopupOpen = true;
        ImGui::OpenPopup("Create Blueprint Node");
    }

    RenderCreatePopup();
    RenderNodeContextMenu();
    RenderLinkContextMenu();
    ed::Resume();

    ed::End();

    const ImRect editorRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::BeginDragDropTargetCustom(editorRect, ImGui::GetID("BlueprintGraphDropTarget"))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")) {
            std::string asset(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
            ApplyAssetToSelection(asset);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();
}

void BlueprintEditor::RenderNode(NodeInstance& node) {
    const bool selected = ed::IsNodeSelected(node.id);
    const ImColor nodeFill = selected ? ImColor(28, 37, 54, 250) : ImColor(19, 24, 38, 240);
    const ImColor nodeBorder = selected ? node.templateRef->accent : ImColor(70, 86, 111, 220);
    ImRect headerRect;

    ed::PushStyleColor(ed::StyleColor_NodeBg, nodeFill);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorder);
    ed::BeginNode(node.id);
    ImGui::PushID(node.id.AsPointer());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));

    ImGui::BeginGroup();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kNodeContentWidth - 24.0f);
    ImGui::TextColored(ImVec4(0.97f, 0.98f, 1.0f, 1.0f), "%s", node.templateRef->title.c_str());
    ImGui::TextColored(ImVec4(0.72f, 0.77f, 0.86f, 0.95f), "%s", node.templateRef->description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    headerRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (ImGui::BeginTable("node-layout", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoKeepColumnsVisible, ImVec2(kNodeContentWidth, 0.0f))) {
        ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthFixed, kInputColumnWidth);
        ImGui::TableSetupColumn("outputs", ImGuiTableColumnFlags_WidthFixed, kOutputColumnWidth);

        const size_t rowCount = std::max(node.inputs.size(), node.outputs.size());
        for (size_t row = 0; row < rowCount; ++row) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (row < node.inputs.size()) {
                RenderPin(node, node.inputs[row], true);
            }
            else {
                ImGui::Dummy(ImVec2(0.0f, 22.0f));
            }

            ImGui::TableSetColumnIndex(1);
            if (row < node.outputs.size()) {
                RenderPin(node, node.outputs[row], false);
            }
            else {
                ImGui::Dummy(ImVec2(0.0f, 22.0f));
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndGroup();

    ImGui::PopStyleVar(3);
    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);

    const ImRect nodeRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImDrawList* background = ed::GetNodeBackgroundDrawList(node.id);
    background->AddRectFilled(nodeRect.Min, nodeRect.Max, nodeFill, kNodeRounding, ImDrawFlags_RoundCornersAll);

    const float halfBorder = ed::GetStyle().NodeBorderWidth * 0.5f;
    const ImVec2 headerMin(nodeRect.Min.x + halfBorder, nodeRect.Min.y + halfBorder);
    const ImVec2 headerMax(nodeRect.Max.x - halfBorder, std::min(nodeRect.Max.y, headerRect.Max.y + 10.0f));
    if (headerMax.x > headerMin.x && headerMax.y > headerMin.y) {
        if (m_headerTexture && m_headerTextureWidth > 0 && m_headerTextureHeight > 0) {
            const ImVec2 uv(
                std::clamp((headerMax.x - headerMin.x) / (kHeaderTextureScale * static_cast<float>(m_headerTextureWidth)), 0.0f, 1.0f), std::clamp((headerMax.y - headerMin.y) / (kHeaderTextureScale * static_cast<float>(m_headerTextureHeight)), 0.0f, 1.0f)
            );
            ImColor headerTint = node.templateRef->accent;
            headerTint.Value.w = selected ? 0.90f : 0.84f;
            background->AddImageRounded(m_headerTexture, headerMin, headerMax, ImVec2(0.0f, 0.0f), uv, headerTint, kNodeRounding, ImDrawFlags_RoundCornersTop);
            background->AddRectFilled(headerMin, headerMax, selected ? IM_COL32(8, 12, 20, 56) : IM_COL32(8, 12, 20, 84), kNodeRounding, ImDrawFlags_RoundCornersTop);
        }
        else {
            ImColor fallback = node.templateRef->accent;
            fallback.Value.w = selected ? 0.50f : 0.38f;
            background->AddRectFilled(headerMin, headerMax, fallback, kNodeRounding, ImDrawFlags_RoundCornersTop);
        }
    }

    background->AddLine(ImVec2(nodeRect.Min.x + 10.0f, headerRect.Max.y + 8.0f), ImVec2(nodeRect.Max.x - 10.0f, headerRect.Max.y + 8.0f), ImColor(255, 255, 255, 36), 1.0f);
    background->AddRect(nodeRect.Min, nodeRect.Max, nodeBorder, kNodeRounding, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.2f);
}

void BlueprintEditor::RenderCreatePopup() {
    if (ImGui::BeginPopup("Create Blueprint Node")) {
        m_createPopupOpen = true;
        ImGui::SetNextItemWidth(-1.0f);
        std::string currentCategory;

        const ImVec2 canvasPosition = ed::ScreenToCanvas(m_createPopupPosition);
        for (const auto& definition : m_library) {
            if (definition.category != currentCategory) {
                currentCategory = definition.category;
                if (!currentCategory.empty()) {
                    ImGui::SeparatorText(currentCategory.c_str());
                }
            }

            if (ImGui::MenuItem(definition.title.c_str(), definition.description.c_str())) {
                NodeInstance* created = AddNode(definition.typeId, canvasPosition);
                if (created && m_pendingLinkPinId) {
                    PinInstance* pendingPin = FindPin(m_pendingLinkPinId);
                    if (pendingPin) {
                        std::vector<PinInstance>& candidatePins = pendingPin->kind == ed::PinKind::Input ? created->outputs : created->inputs;
                        for (const auto& candidate : candidatePins) {
                            const PinInstance* first = pendingPin;
                            const PinInstance* second = &candidate;
                            if (pendingPin->kind == ed::PinKind::Input) {
                                std::swap(first, second);
                            }

                            if (CanCreateLink(first, second)) {
                                AddLink(first->id, second->id);
                                break;
                            }
                        }
                    }
                }

                m_pendingLinkPinId = 0;
                m_createPopupOpen = false;
                MarkSceneDocumentDirty();
                Log("Created node: " + definition.title);
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }
    else {
        m_createPopupOpen = false;
        m_pendingLinkPinId = 0;
    }
}

void BlueprintEditor::RenderNodeContextMenu() {
    if (ImGui::BeginPopup("Node Context Menu")) {
        if (const NodeInstance* node = FindNode(m_contextNodeId)) {
            ImGui::TextUnformatted(node->templateRef->title.c_str());
            ImGui::TextDisabled("%s", node->templateRef->description.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Focus Node")) {
                ed::SelectNode(node->id, true);
                ed::NavigateToSelection();
            }
            if (ImGui::MenuItem("Delete Node")) {
                ed::DeleteNode(node->id);
            }
        }
        ImGui::EndPopup();
    }
}

void BlueprintEditor::RenderLinkContextMenu() {
    if (ImGui::BeginPopup("Link Context Menu")) {
        if (const LinkInstance* link = FindLink(m_contextLinkId)) {
            ImGui::Text("Link %d", static_cast<int>(link->id.Get()));
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Link")) {
                ed::DeleteLink(link->id);
            }
        }
        ImGui::EndPopup();
    }
}

void BlueprintEditor::HandleCreateDeleteInteractions() {
    const bool creating = ed::BeginCreate(ImColor(137, 208, 255), 2.5f);
    if (creating) {
        ed::PinId startPinId = 0;
        ed::PinId endPinId = 0;
        if (ed::QueryNewLink(&startPinId, &endPinId)) {
            const PinInstance* startPin = FindPin(startPinId);
            const PinInstance* endPin = FindPin(endPinId);

            if (startPin && endPin && startPin->kind == ed::PinKind::Input) {
                std::swap(startPin, endPin);
                std::swap(startPinId, endPinId);
            }

            if (CanCreateLink(startPin, endPin)) {
                ImGui::SetTooltip("Create link");
                if (ed::AcceptNewItem(ImColor(148, 255, 177), 3.0f)) {
                    AddLink(startPinId, endPinId);
                    MarkSceneDocumentDirty();
                }
            }
            else {
                ImGui::SetTooltip("Pins are incompatible");
                ed::RejectNewItem(ImColor(255, 110, 110), 2.0f);
            }
        }

        ed::PinId newNodePin = 0;
        if (ed::QueryNewNode(&newNodePin)) {
            ImGui::SetTooltip("Create node");
            if (ed::AcceptNewItem()) {
                m_pendingLinkPinId = newNodePin;
                m_createPopupPosition = ImGui::GetMousePos();
                m_createPopupOpen = true;
                ed::Suspend();
                ImGui::OpenPopup("Create Blueprint Node");
                ed::Resume();
            }
        }
    }
    ed::EndCreate();

    const bool deleting = ed::BeginDelete();
    if (deleting) {
        ed::NodeId deletedNodeId = 0;
        while (ed::QueryDeletedNode(&deletedNodeId)) {
            if (ed::AcceptDeletedItem()) {
                NodeInstance* node = FindNode(deletedNodeId);
                if (!node) {
                    continue;
                }

                std::unordered_set<uintptr_t> pinIds;
                for (const auto& pin : node->inputs) {
                    pinIds.insert(reinterpret_cast<uintptr_t>(pin.id.AsPointer()));
                }
                for (const auto& pin : node->outputs) {
                    pinIds.insert(reinterpret_cast<uintptr_t>(pin.id.AsPointer()));
                }

                m_links.erase(
                    std::remove_if(
                        m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return pinIds.contains(reinterpret_cast<uintptr_t>(link.startPinId.AsPointer())) || pinIds.contains(reinterpret_cast<uintptr_t>(link.endPinId.AsPointer())); }
                    ),
                    m_links.end()
                );

                m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const NodeInstance& candidate) { return candidate.id == deletedNodeId; }), m_nodes.end());
                MarkSceneDocumentDirty();

                if (m_selectedNodeId == deletedNodeId) {
                    m_selectedNodeId = 0;
                }
            }
        }

        ed::LinkId deletedLinkId = 0;
        while (ed::QueryDeletedLink(&deletedLinkId)) {
            if (ed::AcceptDeletedItem()) {
                m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.id == deletedLinkId; }), m_links.end());
                MarkSceneDocumentDirty();

                if (m_selectedLinkId == deletedLinkId) {
                    m_selectedLinkId = 0;
                }
            }
        }
    }
    ed::EndDelete();
}

void BlueprintEditor::RefreshSelectionState() {
    if (!ed::HasSelectionChanged()) {
        return;
    }

    std::array<ed::NodeId, 8> nodeBuffer{};
    const int selectedNodeCount = ed::GetSelectedNodes(nodeBuffer.data(), static_cast<int>(nodeBuffer.size()));
    m_selectedNodeId = selectedNodeCount > 0 ? nodeBuffer.front() : ed::NodeId(0);

    std::array<ed::LinkId, 8> linkBuffer{};
    const int selectedLinkCount = ed::GetSelectedLinks(linkBuffer.data(), static_cast<int>(linkBuffer.size()));
    m_selectedLinkId = selectedLinkCount > 0 ? linkBuffer.front() : ed::LinkId(0);
}

void BlueprintEditor::RenderPin(const NodeInstance& node, const PinInstance& pin, bool inputSide) {
    const bool linked = IsPinLinked(pin.id);
    const ImColor pinColor = GetPinColor(pin.type);
    const IconType iconType = pin.type == PinDataType::Flow ? IconType::Flow : IconType::Circle;

    ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
    ed::PushStyleVar(ed::StyleVar_PivotAlignment, inputSide ? ImVec2(0.0f, 0.5f) : ImVec2(1.0f, 0.5f));
    ed::BeginPin(pin.id, pin.kind);

    if (inputSide) {
        ax::Widgets::Icon(ImVec2(kPinIconSize, kPinIconSize), iconType, linked, pinColor, ImColor(32, 36, 48));
        if (!pin.name.empty()) {
            ImGui::SameLine(0.0f, kPinLabelSpacing);
            ImGui::TextUnformatted(pin.name.c_str());
        }
    }
    else {
        const float labelWidth = pin.name.empty() ? 0.0f : ImGui::CalcTextSize(pin.name.c_str()).x;
        const float totalWidth = labelWidth + (pin.name.empty() ? kPinIconSize : kPinIconSize + kPinLabelSpacing);
        const float offset = std::max(0.0f, kOutputAlignWidth - totalWidth);
        if (offset > 0.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        if (!pin.name.empty()) {
            ImGui::TextUnformatted(pin.name.c_str());
            ImGui::SameLine(0.0f, kPinLabelSpacing);
        }
        ax::Widgets::Icon(ImVec2(kPinIconSize, kPinIconSize), iconType, linked, pinColor, ImColor(32, 36, 48));
    }

    ed::EndPin();
    ed::PopStyleVar(2);

    if (inputSide && !pin.parameterKey.empty() && !linked) {
        NodeInstance* editableNode = FindNode(node.id);
        if (editableNode) {
            if (NodeParameter* parameter = FindParameter(*editableNode, pin.parameterKey)) {
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::Indent(24.0f);
                RenderParameterEditor(*parameter, "_compact", true);
                ImGui::Unindent(24.0f);
            }
        }
    }
}

void BlueprintEditor::RenderParameterEditor(NodeParameter& parameter, std::string_view labelSuffix, bool compact) {
    const std::string controlId = "##" + parameter.key + std::string(labelSuffix);
    bool changed = false;

    if (!compact) {
        ImGui::TextUnformatted(parameter.label.c_str());
        ImGui::SetNextItemWidth(-1.0f);
    }
    else {
        ImGui::SetNextItemWidth(kCompactParameterWidth);
    }

    switch (parameter.kind) {
        case ParameterKind::Boolean:
            changed = ImGui::Checkbox(controlId.c_str(), &parameter.boolValue);
            break;
        case ParameterKind::Integer:
            changed = ImGui::DragInt(controlId.c_str(), &parameter.intValue, parameter.speed, static_cast<int>(parameter.minValue), static_cast<int>(parameter.maxValue));
            break;
        case ParameterKind::Float:
            changed = ImGui::DragFloat(controlId.c_str(), &parameter.floatValue, parameter.speed, parameter.minValue, parameter.maxValue, "%.2f");
            break;
        case ParameterKind::String:
            changed = ImGui::InputText(controlId.c_str(), &parameter.stringValue);
            break;
        case ParameterKind::Asset:
            RenderAssetParameterEditor(parameter, controlId, compact);
            break;
        case ParameterKind::Option:
            if (ImGui::BeginCombo(controlId.c_str(), parameter.stringValue.c_str())) {
                for (const auto& option : parameter.options) {
                    const bool selected = option == parameter.stringValue;
                    if (ImGui::Selectable(option.c_str(), selected)) {
                        parameter.stringValue = option;
                        changed = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            break;
    }

    if (changed) {
        MarkSceneDocumentDirty();
    }
}

void BlueprintEditor::RenderAssetParameterEditor(NodeParameter& parameter, const std::string& controlId, bool compact) {
    const std::string fieldId = controlId + "_asset";
    bool changed = ImGui::InputText(fieldId.c_str(), &parameter.stringValue, ImGuiInputTextFlags_ReadOnly);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")) {
            parameter.stringValue.assign(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }

    const std::string selectedResource = m_selectedResourceCallback ? m_selectedResourceCallback() : std::string{};
    const bool hasSelectedResource = !selectedResource.empty();
    if (!compact) {
        if (!parameter.stringValue.empty()) {
            ImGui::TextDisabled("Bound from project browser.");
        } else {
            ImGui::TextDisabled("Pick from the project browser or drag a resource onto this field.");
        }
    }

    if (!compact) {
        if (!hasSelectedResource) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(("Use Explorer Selection" + controlId).c_str())) {
            parameter.stringValue = selectedResource;
            changed = true;
        }
        if (!hasSelectedResource) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(("Clear" + controlId).c_str())) {
            parameter.stringValue.clear();
            changed = true;
        }
        if (changed) {
            MarkSceneDocumentDirty();
        }
        return;
    }

    if (!hasSelectedResource) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(("Use Selected" + controlId).c_str())) {
        parameter.stringValue = selectedResource;
        changed = true;
    }
    if (!hasSelectedResource) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(("X" + controlId).c_str())) {
        parameter.stringValue.clear();
        changed = true;
    }
    if (changed) {
        MarkSceneDocumentDirty();
    }
}

const BlueprintEditor::NodeTemplate* BlueprintEditor::FindTemplate(const std::string& typeId) const {
    auto it = std::find_if(m_library.begin(), m_library.end(), [&](const NodeTemplate& definition) { return definition.typeId == typeId; });
    return it == m_library.end() ? nullptr : &(*it);
}

BlueprintEditor::NodeInstance* BlueprintEditor::FindNode(ed::NodeId id) {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const NodeInstance& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

const BlueprintEditor::NodeInstance* BlueprintEditor::FindNode(ed::NodeId id) const {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const NodeInstance& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

BlueprintEditor::PinInstance* BlueprintEditor::FindPin(ed::PinId id) {
    for (auto& node : m_nodes) {
        for (auto& pin : node.inputs) {
            if (pin.id == id) {
                return &pin;
            }
        }
        for (auto& pin : node.outputs) {
            if (pin.id == id) {
                return &pin;
            }
        }
    }
    return nullptr;
}

const BlueprintEditor::PinInstance* BlueprintEditor::FindPin(ed::PinId id) const {
    for (const auto& node : m_nodes) {
        for (const auto& pin : node.inputs) {
            if (pin.id == id) {
                return &pin;
            }
        }
        for (const auto& pin : node.outputs) {
            if (pin.id == id) {
                return &pin;
            }
        }
    }
    return nullptr;
}

BlueprintEditor::LinkInstance* BlueprintEditor::FindLink(ed::LinkId id) {
    auto it = std::find_if(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.id == id; });
    return it == m_links.end() ? nullptr : &(*it);
}

const BlueprintEditor::LinkInstance* BlueprintEditor::FindLink(ed::LinkId id) const {
    auto it = std::find_if(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.id == id; });
    return it == m_links.end() ? nullptr : &(*it);
}

BlueprintEditor::NodeParameter* BlueprintEditor::FindParameter(NodeInstance& node, std::string_view key) {
    auto it = std::find_if(node.parameters.begin(), node.parameters.end(), [&](const NodeParameter& parameter) { return parameter.key == key; });
    return it == node.parameters.end() ? nullptr : &(*it);
}

const BlueprintEditor::NodeParameter* BlueprintEditor::FindParameter(const NodeInstance& node, std::string_view key) const {
    auto it = std::find_if(node.parameters.begin(), node.parameters.end(), [&](const NodeParameter& parameter) { return parameter.key == key; });
    return it == node.parameters.end() ? nullptr : &(*it);
}

bool BlueprintEditor::IsPinLinked(ed::PinId id) const {
    return std::any_of(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.startPinId == id || link.endPinId == id; });
}

bool BlueprintEditor::CanCreateLink(const PinInstance* a, const PinInstance* b) const {
    if (!a || !b || a == b) {
        return false;
    }

    if (a->kind != ed::PinKind::Output || b->kind != ed::PinKind::Input) {
        return false;
    }

    if (a->ownerId == b->ownerId) {
        return false;
    }

    if (a->type == b->type) {
        return true;
    }

    const bool assetStringPair = (a->type == PinDataType::Asset && b->type == PinDataType::String) || (a->type == PinDataType::String && b->type == PinDataType::Asset);
    const bool floatIntPair = (a->type == PinDataType::Float && b->type == PinDataType::Int) || (a->type == PinDataType::Int && b->type == PinDataType::Float);

    return assetStringPair || floatIntPair;
}

const BlueprintEditor::PinInstance* BlueprintEditor::ResolveSourcePin(const PinInstance& inputPin) const {
    if (inputPin.kind != ed::PinKind::Input) {
        return nullptr;
    }

    auto it = std::find_if(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.endPinId == inputPin.id; });
    if (it == m_links.end()) {
        return nullptr;
    }

    return FindPin(it->startPinId);
}

const BlueprintEditor::NodeInstance* BlueprintEditor::ResolveSourceNode(const PinInstance& inputPin) const {
    const PinInstance* sourcePin = ResolveSourcePin(inputPin);
    if (!sourcePin) {
        return nullptr;
    }

    return FindNode(ed::NodeId(sourcePin->ownerId));
}

std::string BlueprintEditor::ResolveString(const NodeInstance& node, std::string_view key) const {
    auto pinIt = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const PinInstance& pin) { return pin.parameterKey == key; });

    if (pinIt != node.inputs.end()) {
        if (const NodeInstance* source = ResolveSourceNode(*pinIt)) {
            if (source->templateRef->typeId == "string_literal") {
                if (const NodeParameter* parameter = FindParameter(*source, "value")) {
                    return parameter->stringValue;
                }
            }
            if (source->templateRef->typeId == "asset_reference") {
                if (const NodeParameter* parameter = FindParameter(*source, "path")) {
                    return parameter->stringValue;
                }
            }
        }
    }

    if (const NodeParameter* parameter = FindParameter(node, key)) {
        return parameter->stringValue;
    }
    return {};
}

float BlueprintEditor::ResolveFloat(const NodeInstance& node, std::string_view key) const {
    auto pinIt = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const PinInstance& pin) { return pin.parameterKey == key; });

    if (pinIt != node.inputs.end()) {
        if (const NodeInstance* source = ResolveSourceNode(*pinIt)) {
            if (source->templateRef->typeId == "float_literal") {
                if (const NodeParameter* parameter = FindParameter(*source, "value")) {
                    return parameter->floatValue;
                }
            }
        }
    }

    if (const NodeParameter* parameter = FindParameter(node, key)) {
        return parameter->kind == ParameterKind::Integer ? static_cast<float>(parameter->intValue) : parameter->floatValue;
    }
    return 0.0f;
}

int BlueprintEditor::ResolveInt(const NodeInstance& node, std::string_view key) const {
    if (const NodeParameter* parameter = FindParameter(node, key)) {
        return parameter->kind == ParameterKind::Float ? static_cast<int>(std::round(parameter->floatValue)) : parameter->intValue;
    }
    return static_cast<int>(std::round(ResolveFloat(node, key)));
}

bool BlueprintEditor::ResolveBool(const NodeInstance& node, std::string_view key) const {
    auto pinIt = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const PinInstance& pin) { return pin.parameterKey == key; });

    if (pinIt != node.inputs.end()) {
        if (const NodeInstance* source = ResolveSourceNode(*pinIt)) {
            if (source->templateRef->typeId == "bool_literal") {
                if (const NodeParameter* parameter = FindParameter(*source, "value")) {
                    return parameter->boolValue;
                }
            }
        }
    }

    if (const NodeParameter* parameter = FindParameter(node, key)) {
        return parameter->boolValue;
    }
    return false;
}

const BlueprintEditor::NodeInstance* BlueprintEditor::FindFlowStart() const {
    auto hasIncomingFlow = [&](const NodeInstance& candidate) {
        for (const auto& pin : candidate.inputs) {
            if (pin.type != PinDataType::Flow) {
                continue;
            }
            if (std::any_of(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.endPinId == pin.id; })) {
                return true;
            }
        }
        return false;
    };

    for (const auto& node : m_nodes) {
        const bool hasFlowOut = std::any_of(node.outputs.begin(), node.outputs.end(), [&](const PinInstance& pin) { return pin.type == PinDataType::Flow; });
        if (hasFlowOut && !hasIncomingFlow(node)) {
            return &node;
        }
    }

    return m_nodes.empty() ? nullptr : &m_nodes.front();
}

const BlueprintEditor::NodeInstance* BlueprintEditor::FindFlowNext(const NodeInstance& node) const {
    for (const auto& pin : node.outputs) {
        if (pin.type != PinDataType::Flow) {
            continue;
        }

        auto it = std::find_if(m_links.begin(), m_links.end(), [&](const LinkInstance& link) { return link.startPinId == pin.id; });
        if (it != m_links.end()) {
            if (const PinInstance* targetPin = FindPin(it->endPinId)) {
                return FindNode(ed::NodeId(targetPin->ownerId));
            }
        }
    }

    return nullptr;
}

std::vector<const BlueprintEditor::NodeInstance*> BlueprintEditor::BuildLinearFlow() const {
    std::vector<const NodeInstance*> ordered;
    const NodeInstance* current = FindFlowStart();
    std::unordered_set<uintptr_t> visited;

    while (current && !visited.contains(reinterpret_cast<uintptr_t>(current->id.AsPointer()))) {
        visited.insert(reinterpret_cast<uintptr_t>(current->id.AsPointer()));
        ordered.push_back(current);
        current = FindFlowNext(*current);
    }

    return ordered;
}

ImColor BlueprintEditor::GetPinColor(PinDataType type) const {
    switch (type) {
        case PinDataType::Flow:
            return ImColor(255, 255, 255);
        case PinDataType::Bool:
            return ImColor(255, 110, 110);
        case PinDataType::Int:
            return ImColor(89, 208, 164);
        case PinDataType::Float:
            return ImColor(132, 234, 114);
        case PinDataType::String:
            return ImColor(186, 107, 255);
        case PinDataType::Asset:
            return ImColor(91, 194, 255);
    }
    return ImColor(255, 255, 255);
}

}  // namespace PrismatiX::Editor
