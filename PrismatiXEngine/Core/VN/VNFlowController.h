#pragma once

#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "Core/VN/VNChoiceList.h"
#include "Core/VN/VNDialogueSystem.h"
#include "Core/VN/VNStage.h"
#include "Core/VN/Commands/VNContext.h"

namespace sol {
class state;
}

namespace PrismatiX {
namespace Models {
struct VNCommand;
}
namespace Services {
class ResourceManager;
class GameState;
}
namespace Systems {
class AudioSystem;
}
namespace VN {
class VNScriptParser;
namespace Commands {
class ICommandHandler;
}
}
}  // namespace PrismatiX

namespace PrismatiX {
namespace VN {

class VNFlowController {
public:
    explicit VNFlowController(Commands::VNServices services);
    ~VNFlowController();

    void LoadScript(const std::string& scriptName);
    void Update(int mx, int my);
    void Render();
    void HandleClick(int mx, int my);

    // Getters for S/L and UI
    std::string GetCurrentScript() const { return currentScriptName; }
    int GetCurrentLine() const { return currentLine; }
    void SetCurrentLine(int line) { currentLine = line; }

    VNDialogueSystem& GetDialogueSystem() { return dialogueSystem; }
    VNStage& GetStage() { return stage; }

    void PushPendingBgm(const std::string& name) { pendingBgm.push(name); }
    void PushPendingChapter(const std::string& name) { pendingChapters.push(name); }
    std::string PopPendingBgm();
    std::string PopPendingChapter();

    void SelectChoice(int index);

private:
    Services::ResourceManager& resourceManager;
    VNScriptParser& scriptingEngine;
    Services::GameState& gameState;
    Systems::AudioSystem& audioSystem;
    UI::VNChoiceList& choiceList;
    sol::state& luaState;
    VNDialogueSystem dialogueSystem;
    VNStage stage;

    std::string currentScriptName;
    std::vector<Models::VNCommand> commands;
    int currentLine = 0;
    bool isFinished = false;
    bool hasStarted = false;

    std::queue<std::string> pendingBgm;
    std::queue<std::string> pendingChapters;

    struct PendingJump {
        std::string target;
        bool active = false;
    } pendingJump;

    std::map<std::string, std::unique_ptr<Commands::ICommandHandler>> handlers;
    void InitHandlers();

    void ExecuteNext();
};

}  // namespace VN
}  // namespace PrismatiX
