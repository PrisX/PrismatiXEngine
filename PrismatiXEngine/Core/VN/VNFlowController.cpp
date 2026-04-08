#include "VNFlowController.h"

#include <algorithm>
#include <iostream>

#include "Core/EngineConfig.h"
#include "Utils/Logger.h"

namespace {
std::string GetArg(const std::map<std::string, std::string>& args, const std::string& key, const std::string& def = "") {
    auto it = args.find(key);
    return (it != args.end()) ? it->second : def;
}

int ParseInt(const std::map<std::string, std::string>& args, const std::string& key, int def = 0) {
    auto it = args.find(key);
    if (it == args.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

int FindLabelLine(const std::vector<VNCommand>& commands, const std::string& labelName) {
    for (int i = 0; i < (int)commands.size(); ++i) {
        if (commands[i].type == "label" && GetArg(commands[i].args, "name") == labelName) return i;
    }
    return -1;
}
}  // namespace

VNFlowController::VNFlowController(Engine& engine) : engine(engine), stage(engine.GetResourceManager(), engine.GetRenderSystem()) { InitHandlers(); }

void VNFlowController::InitHandlers() {
    handlers["text"] = [this](const VNCommand& cmd) { CmdText(cmd); };
    handlers["bg"] = [this](const VNCommand& cmd) { CmdBg(cmd); };
    handlers["char"] = [this](const VNCommand& cmd) { CmdChar(cmd); };
    handlers["char_clear"] = [this](const VNCommand& cmd) {
        std::string id = GetArg(cmd.args, "id");
        PX_LOG_DEBUG("[VN] Clear Char: {}", id);
        stage.ClearCharacter(id);
        currentLine++;
    };
    handlers["bgm"] = [this](const VNCommand& cmd) {
        std::string file = GetArg(cmd.args, "file");
        std::string title = GetArg(cmd.args, "title", file);
        PX_LOG_DEBUG("[VN] Play BGM: {} ({})", file, title);
        engine.GetAudioSystem().PlayBGM(file);
        PushPendingBgm(title);
        currentLine++;
    };
    handlers["chapter"] = [this](const VNCommand& cmd) {
        std::string title = GetArg(cmd.args, "title");
        PX_LOG_DEBUG("[VN] Chapter: {}", title);
        PushPendingChapter(title);
        currentLine++;
    };
    handlers["stopbgm"] = [this](const VNCommand& cmd) {
        PX_LOG_DEBUG("[VN] Stop BGM");
        engine.GetAudioSystem().StopBGM();
        currentLine++;
    };
    handlers["se"] = [this](const VNCommand& cmd) {
        PX_LOG_DEBUG("[VN] Play SE: {}", GetArg(cmd.args, "file"));
        engine.GetAudioSystem().PlaySFX(GetArg(cmd.args, "file"));
        currentLine++;
    };
    handlers["var"] = [this](const VNCommand& cmd) { CmdVar(cmd); };
    handlers["jump"] = [this](const VNCommand& cmd) { CmdJump(cmd); };
    handlers["choice"] = [this](const VNCommand& cmd) {
        std::string text = GetArg(cmd.args, "text", GetArg(cmd.args, "content"));
        std::string target = GetArg(cmd.args, "target");
        PX_LOG_DEBUG("[VN] Add Choice: {} -> {}", text, target);
        TTF_Font* font = engine.GetResourceManager().LoadFont("NotoSansTC-Bold.ttf", 24);
        SDL_Color idle = { 255, 255, 255, 255 }, hover = { 255, 200, 0, 255 };
        engine.GetUIManager().AddTextButton(text, font, idle, hover, target);
        currentLine++;
    };
    handlers["if"] = [this](const VNCommand& cmd) {
        std::string v = GetArg(cmd.args, "var"), op = GetArg(cmd.args, "op");
        int val = ParseInt(cmd.args, "val");
        if (engine.GetGameState().CheckFlag(v, op, val)) {
            PX_LOG_DEBUG("[VN] If {} {} {}: TRUE", v, op, val);
            currentLine++;
        }
        else {
            PX_LOG_DEBUG("[VN] If {} {} {}: FALSE, skipping...", v, op, val);
            int depth = 0;
            while (++currentLine < (int)commands.size()) {
                if (commands[currentLine].type == "if")
                    depth++;
                else if (commands[currentLine].type == "else" && depth == 0) {
                    currentLine++;
                    break;
                }
                else if (commands[currentLine].type == "endif") {
                    if (depth == 0) {
                        currentLine++;
                        break;
                    }
                    else
                        depth--;
                }
            }
        }
    };
    handlers["else"] = [this](const VNCommand& cmd) {
        int depth = 0;
        while (++currentLine < (int)commands.size()) {
            if (commands[currentLine].type == "if")
                depth++;
            else if (commands[currentLine].type == "endif") {
                if (depth == 0) {
                    currentLine++;
                    break;
                }
                else
                    depth--;
            }
        }
    };
    handlers["endif"] = [this](const VNCommand& cmd) { currentLine++; };
    handlers["label"] = [this](const VNCommand& cmd) { currentLine++; };
    handlers["lua"] = [this](const VNCommand& cmd) {
        std::string fn = GetArg(cmd.args, "fn");
        if (!fn.empty()) {
            sol::protected_function f = engine.GetLuaState()[fn];
            if (f.valid()) {
                sol::table argsTable = engine.GetLuaState().create_table();
                for (auto const& [key, val] : cmd.args) argsTable[key] = val;
                f(argsTable);
            }
        }
        currentLine++;
    };
}

void VNFlowController::LoadScript(const std::string& scriptName) {
    PX_LOG_INFO("[VN] Loading script: {}", scriptName);
    currentScriptName = scriptName;
    commands = engine.GetScriptingEngine().ParseScript(scriptName);
    currentLine = 0;
    isFinished = false;
    hasStarted = false;
}

void VNFlowController::Update(int mx, int my) {
    if (pendingJump.active) {
        std::string target = pendingJump.target;
        pendingJump.active = false;
        pendingJump.target = "";

        if (!target.empty()) {
            if (target[0] == '*') {
                int line = FindLabelLine(commands, target.substr(1));
                if (line >= 0) currentLine = line;
            } else {
                LoadScript(target);
            }
            ExecuteNext();
        }
        return;
    }

    if (!hasStarted && !commands.empty()) {
        hasStarted = true;
        ExecuteNext();
    }

    dialogueSystem.Update();
    stage.Update();
    if (engine.GetUIManager().HasButtons()) engine.GetUIManager().UpdateHover(mx, my);
}

void VNFlowController::Render() {
    stage.Render();
    if (engine.GetUIManager().HasButtons()) engine.GetUIManager().Render();
}

void VNFlowController::SelectChoice(int index) {
    const auto& buttons = engine.GetUIManager().GetButtons();
    if (index < 0 || index >= (int)buttons.size()) return;

    const auto& btn = buttons[index];
    std::string target = btn.target;
    std::string s = btn.transitionStyle;
    std::string sp = btn.transitionSpeed;
    
    engine.GetUIManager().Clear();

    if (!s.empty()) {
        pendingJump.active = true;
        pendingJump.target = target;
        return;
    }

    if (!target.empty()) {
        if (target[0] == '*') {
            int line = FindLabelLine(commands, target.substr(1));
            if (line >= 0) currentLine = line;
        } else {
            LoadScript(target);
        }
        ExecuteNext();
    }
}

void VNFlowController::HandleClick(int mx, int my) {
    if (isFinished) return;
    std::string target, s, sp, e;
    if (engine.GetUIManager().CheckClick(mx, my, target, s, sp, e)) {
        PX_LOG_INFO("[VN] UI Option Selected: {} with transition {}", target, s);
        engine.GetUIManager().Clear();

        if (!s.empty()) {
            pendingJump.active = true;
            pendingJump.target = target;
            return;
        }

        if (!target.empty()) {
            if (target[0] == '*') {
                int line = FindLabelLine(commands, target.substr(1));
                if (line >= 0) currentLine = line;
            }
            else
                LoadScript(target);
            ExecuteNext();
        }
        return;
    }
    if (engine.GetUIManager().HasButtons()) return;
    engine.GetAudioSystem().StopVoice();
    if (!dialogueSystem.GetState().isFinished)
        dialogueSystem.ShowAll();
    else {
        currentLine++;
        ExecuteNext();
    }
}

void VNFlowController::ExecuteNext() {
    while (currentLine < (int)commands.size()) {
        const VNCommand& cmd = commands[currentLine];
        auto it = handlers.find(cmd.type);
        if (it != handlers.end()) {
            if (cmd.type == "choice") {
                engine.GetUIManager().Clear();
                int next = currentLine;
                while (next < (int)commands.size() && commands[next].type == "choice") {
                    handlers["choice"](commands[next]);
                    next++;
                }
                engine.GetUIManager().RecalculateLayout(EngineConfig::kDefaultScreenWidth, EngineConfig::kDefaultScreenHeight);
                currentLine = next;
                break;
            }
            it->second(cmd);
            if (cmd.type == "text") break;
            if (cmd.type == "jump" && !GetArg(cmd.args, "target").empty() && GetArg(cmd.args, "target")[0] != '*') return;
        }
        else {
            PX_LOG_WARN("[VN] Unknown command: {}", cmd.type);
            currentLine++;
        }
    }
    if (currentLine >= (int)commands.size()) {
        PX_LOG_INFO("[VN] Script Finished: {}", currentScriptName);
        isFinished = true;
    }
}

void VNFlowController::CmdText(const VNCommand& cmd) {
    std::string speaker = GetArg(cmd.args, "speaker");
    std::string text = GetArg(cmd.args, "text", GetArg(cmd.args, "content"));
    size_t start = 0;
    while ((start = text.find('{', start)) != std::string::npos) {
        size_t end = text.find('}', start);
        if (end == std::string::npos) break;
        std::string varName = text.substr(start + 1, end - start - 1);
        std::string val = std::to_string(engine.GetGameState().GetFlag(varName));
        text.replace(start, end - start + 1, val);
        start += val.length();
    }
    int speed = ParseInt(cmd.args, "speed", 40);
    PX_LOG_TRACE("[VN] Text: {} says \"{}\"", speaker, text);
    dialogueSystem.SetText(speaker, text, speed, { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, GetArg(cmd.args, "effect"));
    engine.GetGameState().AddLog(speaker, text);
}

void VNFlowController::CmdBg(const VNCommand& cmd) {
    std::string file = GetArg(cmd.args, "file");
    PX_LOG_DEBUG("[VN] Change BG: {}", file);
    stage.SetBackground(file);
    currentLine++;
}

void VNFlowController::CmdChar(const VNCommand& cmd) {
    std::string id = GetArg(cmd.args, "id");
    std::string exp = GetArg(cmd.args, "expression");
    int pos = ParseInt(cmd.args, "pos", 1);
    PX_LOG_DEBUG("[VN] Set Char: {} ({}) at pos {}", id, exp, pos);
    if (!id.empty()) stage.SetCharacter(id, exp, pos);
    currentLine++;
}

void VNFlowController::CmdVar(const VNCommand& cmd) {
    std::string v = GetArg(cmd.args, "var");
    std::string op = GetArg(cmd.args, "op");
    int val = ParseInt(cmd.args, "val");
    PX_LOG_DEBUG("[VN] Var: {} {} {}", v, op, val);
    if (!v.empty()) {
        if (op == "set")
            engine.GetGameState().SetFlag(v, val);
        else if (op == "add")
            engine.GetGameState().AddFlag(v, val);
    }
    currentLine++;
}

void VNFlowController::CmdJump(const VNCommand& cmd) {
    std::string target = GetArg(cmd.args, "target");
    PX_LOG_INFO("[VN] Jump: {}", target);
    if (target.empty()) {
        currentLine++;
        return;
    }
    if (target[0] == '*') {
        int line = FindLabelLine(commands, target.substr(1));
        if (line >= 0)
            currentLine = line;
        else {
            PX_LOG_ERROR("[VN] Label not found: {}", target);
            currentLine++;
        }
    }
    else {
        LoadScript(target);
    }
}

std::string VNFlowController::PopPendingBgm() {
    if (pendingBgm.empty()) return "";
    std::string s = pendingBgm.front();
    pendingBgm.pop();
    return s;
}

std::string VNFlowController::PopPendingChapter() {
    if (pendingChapters.empty()) return "";
    std::string s = pendingChapters.front();
    pendingChapters.pop();
    return s;
}
