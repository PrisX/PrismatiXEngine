#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <SDL2/SDL.h>
#include "ArchiveManager.h"

struct VNCommand {
    std::string type;
    std::map<std::string, std::string> args;
};

class ScriptManager {
public:
    static std::vector<VNCommand> ParseFile(const std::string& fileName) {
        std::vector<VNCommand> script;

        std::vector<char> buffer = ArchiveManager::ExtractFile(fileName);
        if (buffer.empty()) {
            std::cerr << "Can't extract script from archive: " << fileName << std::endl;
            return script;
        }

        std::string scriptContent(buffer.begin(), buffer.end());
        std::stringstream file(scriptContent);

        std::string line;

        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.substr(0, 2) == "//") continue;

            if (line.front() == '[' && line.back() == ']') {
                std::string content = line.substr(1, line.size() - 2);
                VNCommand cmd;

                size_t firstSpace = content.find(' ');
                if (firstSpace == std::string::npos) {
                    cmd.type = content;
                }
                else {
                    cmd.type = content.substr(0, firstSpace);
                    std::string argsStr = content.substr(firstSpace + 1);

                    size_t pos = 0;
                    while (pos < argsStr.length()) {
                        while (pos < argsStr.length() && argsStr[pos] == ' ') pos++;
                        if (pos >= argsStr.length()) break;

                        size_t eqPos = argsStr.find('=', pos);
                        if (eqPos == std::string::npos) break;

                        std::string key = argsStr.substr(pos, eqPos - pos);
                        pos = eqPos + 1;

                        if (pos < argsStr.length() && argsStr[pos] == '"') {
                            pos++;
                            size_t endQuote = argsStr.find('"', pos);
                            if (endQuote != std::string::npos) {
                                cmd.args[key] = argsStr.substr(pos, endQuote - pos);
                                pos = endQuote + 1;
                            }
                            else {
                                break;
                            }
                        }
                        else {
                            size_t spacePos = argsStr.find(' ', pos);
                            if (spacePos == std::string::npos) spacePos = argsStr.length();
                            cmd.args[key] = argsStr.substr(pos, spacePos - pos);
                            pos = spacePos;
                        }
                    }
                }
                script.push_back(cmd);
            }
        }
        return script;
    }

    static SDL_Color ParseColor(const std::string& colorStr, SDL_Color defaultColor) {
		if (colorStr.empty()) return defaultColor;
		std::stringstream ss(colorStr);
        std::string r, g, b;
        if (std::getline(ss, r, ',') && std::getline(ss, g, ',') && std::getline(ss, b, ',')) {
            try {
                return { (Uint8)std::stoi(r), (Uint8)std::stoi(g), (Uint8)std::stoi(b), 255 };
            } 
            catch (const std::exception& e) {
                std::cerr << "Invalid color format (" << colorStr << "), using default colors instead." << std::endl;
                return defaultColor;
            }
        }
		return defaultColor;
    }
};