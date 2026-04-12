#include "Core/VN/Commands/BuiltinCommandHandlers.h"
#include "Core/VN/Commands/VNContext.h"

#include <charconv>
#include <string>
#include <unordered_map>

#include <sol/sol.hpp>

#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Systems/AudioSystem.h"
#include "Core/VN/VNDialogueSystem.h"
#include "Core/VN/VNStage.h"
#include "Core/VN/VNChoiceList.h"
#include "Utils/Logger.h"

namespace PrismatiX::VN {
namespace Commands {

namespace {

std::string GetArg(const std::unordered_map<std::string, std::string>& args, const std::string& key, const std::string& def = "") {
    auto it = args.find(key);
    return (it != args.end()) ? it->second : def;
}

int ParseInt(const std::unordered_map<std::string, std::string>& args, const std::string& key, int def = 0) {
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
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string speaker = GetArg(cmd.args, "speaker");
        std::string text = GetArg(cmd.args, "text", GetArg(cmd.args, "content"));
        size_t start = 0;
        while ((start = text.find('{', start)) != std::string::npos) {
            size_t end = text.find('}', start);
            if (end == std::string::npos) break;
            std::string varName = text.substr(start + 1, end - start - 1);
            std::string value = std::to_string(ctx.services.gameState.GetFlag(varName));
            text.replace(start, end - start + 1, value);
            start += value.length();
        }

        int speed = ParseInt(cmd.args, "speed", 40);
        PX_LOG_TRACE("[VN] Text: {} says \"{}\"", speaker, text);
        ctx.world.dialogueSystem.SetText(speaker, text, speed, { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, GetArg(cmd.args, "effect"));
        ctx.services.gameState.AddLog(speaker, text);
    }
};

class BgCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string file = GetArg(cmd.args, "file");
        PX_LOG_DEBUG("[VN] Change BG: {}", file);
        ctx.world.stage.SetBackground(file);
        ctx.script.currentLine++;
    }
};

class CharCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string id = GetArg(cmd.args, "id");
        std::string expression = GetArg(cmd.args, "expression");
        int pos = ParseInt(cmd.args, "pos", 1);
        PX_LOG_DEBUG("[VN] Set Char: {} ({}) at pos {}", id, expression, pos);
        if (!id.empty()) ctx.world.stage.SetCharacter(id, expression, pos);
        ctx.script.currentLine++;
    }
};

class CharClearCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string id = GetArg(cmd.args, "id");
        PX_LOG_DEBUG("[VN] Clear Char: {}", id);
        ctx.world.stage.ClearCharacter(id);
        ctx.script.currentLine++;
    }
};

class BgmCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string file = GetArg(cmd.args, "file");
        std::string title = GetArg(cmd.args, "title", file);
        PX_LOG_DEBUG("[VN] Play BGM: {} ({})", file, title);
        ctx.services.audioSystem.PlayBGM(file);
        ctx.script.pendingBgm = title;
        ctx.script.currentLine++;
    }
};

class ChapterCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string title = GetArg(cmd.args, "title");
        PX_LOG_DEBUG("[VN] Chapter: {}", title);
        ctx.script.pendingChapters = title;
        ctx.script.currentLine++;
    }
};

class StopBgmCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand&, VNContext& ctx) override {
        PX_LOG_DEBUG("[VN] Stop BGM");
        ctx.services.audioSystem.StopBGM();
        ctx.script.currentLine++;
    }
};

class SeCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string file = GetArg(cmd.args, "file");
        PX_LOG_DEBUG("[VN] Play SE: {}", file);
        ctx.services.audioSystem.PlaySFX(file);
        ctx.script.currentLine++;
    }
};

class VarCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string var = GetArg(cmd.args, "var");
        std::string op = GetArg(cmd.args, "op");
        int value = ParseInt(cmd.args, "val");
        PX_LOG_DEBUG("[VN] Var: {} {} {}", var, op, value);

        if (!var.empty()) {
            if (op == "set") {
                ctx.services.gameState.SetFlag(var, value);
            } else if (op == "add") {
                ctx.services.gameState.AddFlag(var, value);
            }
        }

        ctx.script.currentLine++;
    }
};

class JumpCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string target = GetArg(cmd.args, "target");
        PX_LOG_INFO("[VN] Jump: {}", target);

        if (target.empty()) {
            ctx.script.currentLine++;
            return;
        }

        if (target[0] == '*') {
            int line = ctx.script.findLabelLine(target.substr(1));
            if (line >= 0) {
                ctx.script.currentLine = line;
            } else {
                PX_LOG_ERROR("[VN] Label not found: {}", target);
                ctx.script.currentLine++;
            }
            return;
        }

        ctx.script.loadScript(target);
    }
};

class ChoiceCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string text = GetArg(cmd.args, "text", GetArg(cmd.args, "content"));
        std::string target = GetArg(cmd.args, "target");
        std::string style = GetArg(cmd.args, "style");
        PX_LOG_DEBUG("[VN] Add Choice: {} -> {}", text, target);
        ctx.world.choiceList.AddChoice(text, target, style);
        ctx.script.currentLine++;
    }
};

class IfCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string var = GetArg(cmd.args, "var");
        std::string op = GetArg(cmd.args, "op");
        int value = ParseInt(cmd.args, "val");

        if (ctx.services.gameState.CheckFlag(var, op, value)) {
            PX_LOG_DEBUG("[VN] If {} {} {}: TRUE", var, op, value);
            ctx.script.currentLine++;
            return;
        }

        PX_LOG_DEBUG("[VN] If {} {} {}: FALSE, skipping...", var, op, value);
        int depth = 0;
        while (++ctx.script.currentLine < static_cast<int>(ctx.script.commands.size())) {
            if (ctx.script.commands[ctx.script.currentLine].type == "if") {
                depth++;
            } else if (ctx.script.commands[ctx.script.currentLine].type == "else" && depth == 0) {
                ctx.script.currentLine++;
                break;
            } else if (ctx.script.commands[ctx.script.currentLine].type == "endif") {
                if (depth == 0) {
                    ctx.script.currentLine++;
                    break;
                }
                depth--;
            }
        }
    }
};

class ElseCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand&, VNContext& ctx) override {
        int depth = 0;
        while (++ctx.script.currentLine < static_cast<int>(ctx.script.commands.size())) {
            if (ctx.script.commands[ctx.script.currentLine].type == "if") {
                depth++;
            } else if (ctx.script.commands[ctx.script.currentLine].type == "endif") {
                if (depth == 0) {
                    ctx.script.currentLine++;
                    break;
                }
                depth--;
            }
        }
    }
};

class EndIfCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand&, VNContext& ctx) override {
        ctx.script.currentLine++;
    }
};

class LabelCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand&, VNContext& ctx) override {
        ctx.script.currentLine++;
    }
};

class LuaCommandHandler final : public ICommandHandler {
public:
    void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) override {
        std::string fn = GetArg(cmd.args, "fn");
        if (!fn.empty()) {
            sol::protected_function function = ctx.services.luaState[fn];
            if (function.valid()) {
                sol::table argsTable = ctx.services.luaState.create_table();
                for (const auto& [key, value] : cmd.args) {
                    argsTable[key] = value;
                }
                function(argsTable);
            }
        }
        ctx.script.currentLine++;
    }
};

}  // namespace

std::unordered_map<std::string, std::unique_ptr<ICommandHandler>> CreateBuiltinHandlers() {
    std::unordered_map<std::string, std::unique_ptr<ICommandHandler>> handlers;
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
} // namespace PrismatiX::VN
