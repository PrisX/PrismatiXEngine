#include "Core/VN/Commands/BuiltinCommandHandlers.h"
#include "Core/VN/Commands/VNContext.h"

#include <charconv>
#include <map>
#include <string>

#include <sol/sol.hpp>

#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Systems/AudioSystem.h"
#include "Core/VN/VNDialogueSystem.h"
#include "Core/VN/VNStage.h"
#include "UI/UIManager.h"
#include "Utils/Logger.h"

namespace PrismatiX {
namespace VN {
namespace Commands {

namespace {

std::string GetArg(const std::map<std::string, std::string>& args, const std::string& key, const std::string& def = "") {
    auto it = args.find(key);
    return (it != args.end()) ? it->second : def;
}

int ParseInt(const std::map<std::string, std::string>& args, const std::string& key, int def = 0) {
    auto it = args.find(key);
    if (it == args.end()) return def;
    int parsed = def;
    const char* begin = it->second.data();
    const char* end = begin + it->second.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) return def;
    return parsed;
}

class TextCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string speaker = GetArg(cmd.args, "speaker");
        std::string text = GetArg(cmd.args, "text", GetArg(cmd.args, "content"));
        size_t start = 0;
        while ((start = text.find('{', start)) != std::string::npos) {
            size_t end = text.find('}', start);
            if (end == std::string::npos) break;
            std::string varName = text.substr(start + 1, end - start - 1);
            std::string value = std::to_string(ctx.gameState.GetFlag(varName));
            text.replace(start, end - start + 1, value);
            start += value.length();
        }

        int speed = ParseInt(cmd.args, "speed", 40);
        PX_LOG_TRACE("[VN] Text: {} says \"{}\"", speaker, text);
        ctx.dialogueSystem.SetText(speaker, text, speed, { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, GetArg(cmd.args, "effect"));
        ctx.gameState.AddLog(speaker, text);
    }
};

class BgCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string file = GetArg(cmd.args, "file");
        PX_LOG_DEBUG("[VN] Change BG: {}", file);
        ctx.stage.SetBackground(file);
        ctx.currentLine++;
    }
};

class CharCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string id = GetArg(cmd.args, "id");
        std::string expression = GetArg(cmd.args, "expression");
        int pos = ParseInt(cmd.args, "pos", 1);
        PX_LOG_DEBUG("[VN] Set Char: {} ({}) at pos {}", id, expression, pos);
        if (!id.empty()) ctx.stage.SetCharacter(id, expression, pos);
        ctx.currentLine++;
    }
};

class CharClearCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string id = GetArg(cmd.args, "id");
        PX_LOG_DEBUG("[VN] Clear Char: {}", id);
        ctx.stage.ClearCharacter(id);
        ctx.currentLine++;
    }
};

class BgmCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string file = GetArg(cmd.args, "file");
        std::string title = GetArg(cmd.args, "title", file);
        PX_LOG_DEBUG("[VN] Play BGM: {} ({})", file, title);
        ctx.audioSystem.PlayBGM(file);
        ctx.pendingBgm.push(title);
        ctx.currentLine++;
    }
};

class ChapterCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string title = GetArg(cmd.args, "title");
        PX_LOG_DEBUG("[VN] Chapter: {}", title);
        ctx.pendingChapters.push(title);
        ctx.currentLine++;
    }
};

class StopBgmCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand&, VNContext& ctx) override {
        PX_LOG_DEBUG("[VN] Stop BGM");
        ctx.audioSystem.StopBGM();
        ctx.currentLine++;
    }
};

class SeCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string file = GetArg(cmd.args, "file");
        PX_LOG_DEBUG("[VN] Play SE: {}", file);
        ctx.audioSystem.PlaySFX(file);
        ctx.currentLine++;
    }
};

class VarCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string var = GetArg(cmd.args, "var");
        std::string op = GetArg(cmd.args, "op");
        int value = ParseInt(cmd.args, "val");
        PX_LOG_DEBUG("[VN] Var: {} {} {}", var, op, value);

        if (!var.empty()) {
            if (op == "set") {
                ctx.gameState.SetFlag(var, value);
            } else if (op == "add") {
                ctx.gameState.AddFlag(var, value);
            }
        }

        ctx.currentLine++;
    }
};

class JumpCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string target = GetArg(cmd.args, "target");
        PX_LOG_INFO("[VN] Jump: {}", target);

        if (target.empty()) {
            ctx.currentLine++;
            return;
        }

        if (target[0] == '*') {
            int line = ctx.findLabelLine(target.substr(1));
            if (line >= 0) {
                ctx.currentLine = line;
            } else {
                PX_LOG_ERROR("[VN] Label not found: {}", target);
                ctx.currentLine++;
            }
            return;
        }

        ctx.loadScript(target);
    }
};

class ChoiceCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string text = GetArg(cmd.args, "text", GetArg(cmd.args, "content"));
        std::string target = GetArg(cmd.args, "target");
        PX_LOG_DEBUG("[VN] Add Choice: {} -> {}", text, target);
        TTF_Font* font = ctx.resourceManager.LoadFont("NotoSansTC-Bold.ttf", 24);
        SDL_Color idle = { 255, 255, 255, 255 };
        SDL_Color hover = { 255, 200, 0, 255 };
        ctx.uiManager.AddTextButton(text, font, idle, hover, target);
        ctx.currentLine++;
    }
};

class IfCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string var = GetArg(cmd.args, "var");
        std::string op = GetArg(cmd.args, "op");
        int value = ParseInt(cmd.args, "val");

        if (ctx.gameState.CheckFlag(var, op, value)) {
            PX_LOG_DEBUG("[VN] If {} {} {}: TRUE", var, op, value);
            ctx.currentLine++;
            return;
        }

        PX_LOG_DEBUG("[VN] If {} {} {}: FALSE, skipping...", var, op, value);
        int depth = 0;
        while (++ctx.currentLine < static_cast<int>(ctx.commands.size())) {
            if (ctx.commands[ctx.currentLine].type == "if") {
                depth++;
            } else if (ctx.commands[ctx.currentLine].type == "else" && depth == 0) {
                ctx.currentLine++;
                break;
            } else if (ctx.commands[ctx.currentLine].type == "endif") {
                if (depth == 0) {
                    ctx.currentLine++;
                    break;
                }
                depth--;
            }
        }
    }
};

class ElseCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand&, VNContext& ctx) override {
        int depth = 0;
        while (++ctx.currentLine < static_cast<int>(ctx.commands.size())) {
            if (ctx.commands[ctx.currentLine].type == "if") {
                depth++;
            } else if (ctx.commands[ctx.currentLine].type == "endif") {
                if (depth == 0) {
                    ctx.currentLine++;
                    break;
                }
                depth--;
            }
        }
    }
};

class EndIfCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand&, VNContext& ctx) override {
        ctx.currentLine++;
    }
};

class LabelCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand&, VNContext& ctx) override {
        ctx.currentLine++;
    }
};

class LuaCommandHandler final : public ICommandHandler {
public:
    void Execute(const Models::VNCommand& cmd, VNContext& ctx) override {
        std::string fn = GetArg(cmd.args, "fn");
        if (!fn.empty()) {
            sol::protected_function function = ctx.luaState[fn];
            if (function.valid()) {
                sol::table argsTable = ctx.luaState.create_table();
                for (const auto& [key, value] : cmd.args) {
                    argsTable[key] = value;
                }
                function(argsTable);
            }
        }
        ctx.currentLine++;
    }
};

}  // namespace

std::map<std::string, std::unique_ptr<ICommandHandler>> CreateBuiltinHandlers() {
    std::map<std::string, std::unique_ptr<ICommandHandler>> handlers;
    handlers["text"] = std::make_unique<TextCommandHandler>();
    handlers["bg"] = std::make_unique<BgCommandHandler>();
    handlers["char"] = std::make_unique<CharCommandHandler>();
    handlers["char_clear"] = std::make_unique<CharClearCommandHandler>();
    handlers["bgm"] = std::make_unique<BgmCommandHandler>();
    handlers["chapter"] = std::make_unique<ChapterCommandHandler>();
    handlers["stopbgm"] = std::make_unique<StopBgmCommandHandler>();
    handlers["se"] = std::make_unique<SeCommandHandler>();
    handlers["var"] = std::make_unique<VarCommandHandler>();
    handlers["jump"] = std::make_unique<JumpCommandHandler>();
    handlers["choice"] = std::make_unique<ChoiceCommandHandler>();
    handlers["if"] = std::make_unique<IfCommandHandler>();
    handlers["else"] = std::make_unique<ElseCommandHandler>();
    handlers["endif"] = std::make_unique<EndIfCommandHandler>();
    handlers["label"] = std::make_unique<LabelCommandHandler>();
    handlers["lua"] = std::make_unique<LuaCommandHandler>();
    return handlers;
}

}  // namespace Commands
}  // namespace VN
}  // namespace PrismatiX
