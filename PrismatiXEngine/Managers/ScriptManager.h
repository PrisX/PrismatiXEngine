#pragma once
#include <SDL2/SDL.h>

#include <map>
#include <string>
#include <vector>

struct VNCommand {
    std::string type;
    std::map<std::string, std::string> args;
};

class ScriptManager {
public:
    static std::vector<VNCommand> ParseFile(const std::string& fileName);
    static SDL_Color ParseColor(const std::string& colorStr, SDL_Color defaultColor);
};