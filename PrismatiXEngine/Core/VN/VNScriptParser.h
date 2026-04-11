#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

#include "Core/Models/VNCommand.h"

namespace PrismatiX::Services {
class ResourceManager;
}

namespace PrismatiX::VN {

std::vector<PrismatiX::Models::VNCommand> ParseScript(PrismatiX::Services::ResourceManager& resourceManager, const std::string& fileName);
SDL_Color ParseColor(const std::string& colorStr, SDL_Color defaultColor);

} // namespace PrismatiX::VN
