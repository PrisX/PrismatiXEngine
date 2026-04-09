#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

#include "Core/Models/VNCommand.h"

namespace PrismatiX {
namespace Services {
class ResourceManager;
}

namespace VN {

class VNScriptParser {
public:
    explicit VNScriptParser(Services::ResourceManager& resMgr);
    ~VNScriptParser() = default;

    std::vector<Models::VNCommand> ParseScript(const std::string& fileName);
    SDL_Color ParseColor(const std::string& colorStr, SDL_Color defaultColor);

private:
    Services::ResourceManager& resourceManager;
};

}  // namespace VN
}  // namespace PrismatiX
