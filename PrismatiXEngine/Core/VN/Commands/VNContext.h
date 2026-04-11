#pragma once
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "Core/Models/VNCommand.h"

namespace sol {
class state;
}

namespace PrismatiX::Services {
class ResourceManager;
class GameState;
}

namespace PrismatiX::Systems {
class AudioSystem;
class RenderSystem;
}

namespace PrismatiX::VN {
class VNStage;
class VNDialogueSystem;
class VNChoiceList;
namespace Commands {

struct VNServices {
    PrismatiX::Services::ResourceManager& resourceManager;
    PrismatiX::Services::GameState& gameState;
    PrismatiX::Systems::AudioSystem& audioSystem;
    PrismatiX::Systems::RenderSystem& renderSystem;
    VNChoiceList& choiceList;
    sol::state& luaState;
};

struct VNScriptCtx {
    std::vector<PrismatiX::Models::VNCommand>& commands;
    int& currentLine;
    std::queue<std::string>& pendingBgm;
    std::queue<std::string>& pendingChapters;
    std::function<void(const std::string&)> loadScript;
    std::function<int(const std::string&)> findLabelLine;
};

struct VNWorldCtx {
    VNStage& stage;
    VNDialogueSystem& dialogueSystem;
    VNChoiceList& choiceList;
};

struct VNServiceCtx {
    PrismatiX::Services::ResourceManager& resourceManager;
    PrismatiX::Services::GameState& gameState;
    PrismatiX::Systems::AudioSystem& audioSystem;
    sol::state& luaState;
};

struct VNContext {
    VNScriptCtx script;
    VNWorldCtx world;
    VNServiceCtx services;
};

}  // namespace Commands
} // namespace PrismatiX::VN