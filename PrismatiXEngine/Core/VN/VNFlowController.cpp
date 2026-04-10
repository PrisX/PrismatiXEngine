#include "VNFlowController.h"

#include "Core/EngineConfig.h"
#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Systems/AudioSystem.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/VN/Commands/BuiltinCommandHandlers.h"
#include "Core/VN/Commands/VNContext.h"
#include "Core/Models/VNModels.h"
#include "Core/VN/VNDialogueSystem.h"
#include "Core/VN/VNScriptParser.h"
#include "Core/VN/VNStage.h"
#include "Core/VN/Commands/ICommandHandler.h"
#include "Core/VN/VNChoiceList.h"
#include "Utils/Logger.h"

namespace PrismatiX {
namespace VN {

namespace {
int FindLabelLine(const std::vector<Models::VNCommand>& commands, const std::string& labelName) {
    for (int i = 0; i < (int)commands.size(); ++i) {
        if (commands[i].type != "label") continue;
        auto nameIt = commands[i].args.find("name");
        if (nameIt != commands[i].args.end() && nameIt->second == labelName) return i;
    }
    return -1;
}
}  // namespace

VNFlowController::VNFlowController(Commands::VNServices s)
    : resourceManager(s.resourceManager),
      scriptingEngine(s.scriptingEngine),
      gameState(s.gameState),
      audioSystem(s.audioSystem),
      choiceList(s.choiceList),
      luaState(s.luaState),
      stage(s.resourceManager, s.renderSystem) {
    InitHandlers();
}

VNFlowController::~VNFlowController() = default;

void VNFlowController::InitHandlers() {
    handlers = Commands::CreateBuiltinHandlers();
}

void VNFlowController::LoadScript(const std::string& scriptName) {
    PX_LOG_INFO("[VN] Loading script: {}", scriptName);
    currentScriptName = scriptName;
    commands = scriptingEngine.ParseScript(scriptName);
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
}

void VNFlowController::Render() {
    stage.Render();
}

void VNFlowController::SelectChoice(int index) {
    const auto& choices = choiceList.GetChoices();
    if (index < 0 || index >= (int)choices.size()) return;

    const auto& choice = choices[index];
    std::string target = choice.target;
    std::string s = choice.transitionStyle;
    
    choiceList.Clear();

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
    if (choiceList.HasChoices()) return; // Logic moved to Lua, C++ just blocks script progress

    audioSystem.StopVoice();
    if (!dialogueSystem.GetState().isFinished)
        dialogueSystem.ShowAll();
    else {
        currentLine++;
        ExecuteNext();
    }
}

void VNFlowController::ExecuteNext() {
    Commands::VNContext context{
        resourceManager,
        gameState,
        audioSystem,
        choiceList,
        stage,
        dialogueSystem,
        luaState,
        commands,
        currentLine,
        pendingBgm,
        pendingChapters,
        [this](const std::string& scriptName) { LoadScript(scriptName); },
        [this](const std::string& labelName) { return FindLabelLine(commands, labelName); }
    };

    while (currentLine < (int)commands.size()) {
        const Models::VNCommand& cmd = commands[currentLine];
        auto it = handlers.find(cmd.type);
        if (it != handlers.end()) {
            if (cmd.type == "choice") {
                choiceList.Clear();
                int next = currentLine;
                while (next < (int)commands.size() && commands[next].type == "choice") {
                    it->second->Execute(commands[next], context);
                    next++;
                }
                currentLine = next;
                break;
            }
            it->second->Execute(cmd, context);
            if (cmd.type == "text") break;
            if (cmd.type == "jump") {
                auto targetIt = cmd.args.find("target");
                if (targetIt != cmd.args.end() && !targetIt->second.empty() && targetIt->second[0] != '*') return;
            }
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

}  // namespace VN
}  // namespace PrismatiX
