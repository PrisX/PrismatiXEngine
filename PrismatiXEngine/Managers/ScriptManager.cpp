#include "ScriptManager.h"

#include <cctype>
#include <iostream>
#include <sstream>

#include "ArchiveManager.h"

namespace {
std::string ToLowerCopy(const std::string& value) {
    std::string lowered = value;
    for (char& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

std::string CanonicalCommandType(const std::string& rawType) { return ToLowerCopy(rawType); }

void CanonicalizeCommandArgs(VNCommand& cmd, const std::string& rawType) {
    (void)cmd;
    (void)rawType;
}
}  // namespace

ScriptManager::ScriptManager(ArchiveManager& archiveMgr) : archiveManager(archiveMgr) {}

std::vector<VNCommand> ScriptManager::ParseFile(const std::string& fileName) {
    std::vector<VNCommand> script;

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) {
        std::cerr << "Can't extract script from archive: " << fileName << std::endl;
        return script;
    }

    std::string scriptContent(buffer.begin(), buffer.end());
    std::stringstream file(scriptContent);

    std::string line;
    std::map<std::string, std::string> lastTextArgs;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t startPos = line.find_first_not_of(" \t");
        if (startPos == std::string::npos) continue;
        std::string cleanLine = line.substr(startPos);
        bool isHashComment = false;
        if (!cleanLine.empty() && cleanLine[0] == '#') {
            isHashComment = (cleanLine.size() == 1 || std::isspace(static_cast<unsigned char>(cleanLine[1])));
        }

        if (cleanLine.empty() || cleanLine.substr(0, 2) == "//" || isHashComment) continue;
        if (cleanLine.front() == '*') {
            VNCommand cmd;
            cmd.type = "label";
            cmd.args["name"] = cleanLine.substr(1);
            script.push_back(cmd);
            continue;
        }
        if (cleanLine.front() == '[' && cleanLine.back() == ']') {
            std::string content = cleanLine.substr(1, cleanLine.size() - 2);
            VNCommand cmd;
            std::string rawType;

            size_t firstSpace = content.find(' ');
            if (firstSpace == std::string::npos) {
                rawType = ToLowerCopy(content);
                cmd.type = CanonicalCommandType(content);
            }
            else {
                rawType = ToLowerCopy(content.substr(0, firstSpace));
                cmd.type = CanonicalCommandType(content.substr(0, firstSpace));
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
            CanonicalizeCommandArgs(cmd, rawType);

            if (cmd.type == "text") {
                lastTextArgs = cmd.args;
                lastTextArgs.erase("voice");
                lastTextArgs.erase("content");
                if (cmd.args.count("content") == 0) {
                    continue;
                }
            }

            script.push_back(cmd);
        }
        else {
            VNCommand cmd;
            cmd.type = "text";
            cmd.args = lastTextArgs;
            cmd.args["content"] = cleanLine;

            script.push_back(cmd);
        }
    }
    return script;
}

SDL_Color ScriptManager::ParseColor(const std::string& colorStr, SDL_Color defaultColor) {
    if (colorStr.empty()) return defaultColor;
    std::stringstream ss(colorStr);
    std::string r, g, b;
    if (std::getline(ss, r, ',') && std::getline(ss, g, ',') && std::getline(ss, b, ',')) {
        try {
            return { (Uint8)std::stoi(r), (Uint8)std::stoi(g), (Uint8)std::stoi(b), 255 };
        } catch (const std::exception& e) {
            std::cerr << "Invalid color format (" << colorStr << "), using default colors instead." << std::endl;
            return defaultColor;
        }
    }
    return defaultColor;
}
