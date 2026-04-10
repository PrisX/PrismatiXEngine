#pragma once
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "Core/Models/VNCommand.h"

namespace sol {
class state;
}

namespace PrismatiX {
namespace Services {
class ResourceManager;
class GameState;
}

namespace Systems {
class AudioSystem;
class RenderSystem;
}

namespace UI {
class VNChoiceList;
}

namespace VN {
class VNStage;
class VNDialogueSystem;
class VNScriptParser;

namespace Commands {

struct VNServices {
    Services::ResourceManager& resourceManager;
    VNScriptParser& scriptingEngine;
    Services::GameState& gameState;
    Systems::AudioSystem& audioSystem;
    Systems::RenderSystem& renderSystem;
    UI::VNChoiceList& choiceList;
    sol::state& luaState;
};

struct VNContext {
    Services::ResourceManager& resourceManager;
    Services::GameState& gameState;
    Systems::AudioSystem& audioSystem;
    UI::VNChoiceList& choiceList;
    VN::VNStage& stage;
    VN::VNDialogueSystem& dialogueSystem;
    sol::state& luaState;
    std::vector<Models::VNCommand>& commands;
    int& currentLine;
    std::queue<std::string>& pendingBgm;
    std::queue<std::string>& pendingChapters;
    std::function<void(const std::string&)> loadScript;
    std::function<int(const std::string&)> findLabelLine;
};

}  // namespace Commands
}  // namespace VN
}  // namespace PrismatiX
