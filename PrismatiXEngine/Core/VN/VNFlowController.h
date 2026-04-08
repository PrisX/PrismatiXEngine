#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <queue>

#include "Engine.h"
#include "Services/VNScriptParser.h"
#include "VNDialogueSystem.h"
#include "VNStage.h"
#include "Models.h"

class VNFlowController {
public:
    VNFlowController(Engine& engine);
    ~VNFlowController() = default;

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
    Engine& engine;
    VNDialogueSystem dialogueSystem;
    VNStage stage;

    std::string currentScriptName;
    std::vector<VNCommand> commands;
    int currentLine = 0;
    bool isFinished = false;
    bool hasStarted = false;

    std::queue<std::string> pendingBgm;
    std::queue<std::string> pendingChapters;

    struct PendingJump {
        std::string target;
        bool active = false;
    } pendingJump;

    // Command Handlers
    using CommandHandler = std::function<void(const VNCommand&)>;
    std::map<std::string, CommandHandler> handlers;
    void InitHandlers();

    void ExecuteNext();

    void CmdText(const VNCommand& cmd);
    void CmdBg(const VNCommand& cmd);
    void CmdChar(const VNCommand& cmd);
    void CmdVar(const VNCommand& cmd);
    void CmdJump(const VNCommand& cmd);
};
