#pragma once

#include <SDL2/SDL.h>
#include <map>
#include <string>
#include <vector>

struct VNCommand {
    std::string type;
    std::map<std::string, std::string> args;
};

class ResourceManager;

class VNScriptParser {
public:
    VNScriptParser(ResourceManager& resMgr);
    ~VNScriptParser() = default;

    std::vector<VNCommand> ParseScript(const std::string& fileName);
    SDL_Color ParseColor(const std::string& colorStr, SDL_Color defaultColor);

private:
    ResourceManager& resourceManager;
};
