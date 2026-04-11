#pragma once

#include <unordered_map>
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

namespace PrismatiX::Models {
struct VNCommand;
}
namespace PrismatiX::Services {
class ResourceManager;
class GameState;
}
namespace PrismatiX::Systems {
class AudioSystem;
}
namespace PrismatiX::VN::Commands {
class ICommandHandler;
}

namespace PrismatiX::VN {

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
    PrismatiX::Services::ResourceManager& resourceManager;
    PrismatiX::Services::GameState& gameState;
    PrismatiX::Systems::AudioSystem& audioSystem;
    PrismatiX::VN::VNChoiceList& choiceList;
    sol::state& luaState;
    VNDialogueSystem dialogueSystem;
    VNStage stage;

    std::string currentScriptName;
    std::vector<PrismatiX::Models::VNCommand> commands;
    int currentLine = 0;
    bool isFinished = false;
    bool hasStarted = false;

    std::queue<std::string> pendingBgm;
    std::queue<std::string> pendingChapters;

    struct PendingJump {
        std::string target;
        bool active = false;
    } pendingJump;

    std::unordered_map<std::string, std::unique_ptr<Commands::ICommandHandler>> handlers;
    void InitHandlers();

    void ExecuteNext();
};

} // namespace PrismatiX::VN