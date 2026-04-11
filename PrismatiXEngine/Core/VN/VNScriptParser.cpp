#include "Core/VN/VNScriptParser.h"

#include <cctype>
#include <iostream>
#include <sstream>
#include <map>

#include "Core/Services/ResourceManager.h"

namespace PrismatiX::VN {

namespace {
std::string ToLowerCopy(const std::string& value) {
    std::string lowered = value;
    for (char& ch : lowered) ch = (char)std::tolower((unsigned char)ch);
    return lowered;
}
}  // namespace

std::vector<PrismatiX::Models::VNCommand> ParseScript(PrismatiX::Services::ResourceManager& resourceManager, const std::string& fileName) {
    std::vector<PrismatiX::Models::VNCommand> script;
    std::vector<char> buffer = resourceManager.ExtractFile(fileName);
    if (buffer.empty()) return script;

    std::string content(buffer.begin(), buffer.end());
    std::stringstream file(content);
    std::string line;
    std::unordered_map<std::string, std::string> lastTextArgs;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string clean = line.substr(start);

        if (clean.empty() || clean.substr(0, 2) == "//" || clean[0] == '#') continue;

        if (clean[0] == '*') {
            PrismatiX::Models::VNCommand cmd;
            cmd.type = "label";
            cmd.args["name"] = clean.substr(1);
            script.push_back(cmd);
            continue;
        }

        if (clean[0] == '[' && clean.back() == ']') {
            std::string body = clean.substr(1, clean.size() - 2);
            PrismatiX::Models::VNCommand cmd;
            size_t firstSpace = body.find(' ');
            if (firstSpace == std::string::npos) {
                cmd.type = ToLowerCopy(body);
            }
            else {
                cmd.type = ToLowerCopy(body.substr(0, firstSpace));
                std::string argsStr = body.substr(firstSpace + 1);
                size_t pos = 0;
                while (pos < argsStr.length()) {
                    while (pos < argsStr.length() && argsStr[pos] == ' ') pos++;
                    if (pos >= argsStr.length()) break;
                    size_t eq = argsStr.find('=', pos);
                    if (eq == std::string::npos) break;
                    std::string key = argsStr.substr(pos, eq - pos);
                    pos = eq + 1;
                    if (pos < argsStr.length() && argsStr[pos] == '"') {
                        pos++;
                        size_t endQ = argsStr.find('"', pos);
                        if (endQ != std::string::npos) {
                            cmd.args[key] = argsStr.substr(pos, endQ - pos);
                            pos = endQ + 1;
                        }
                        else
                            break;
                    }
                    else {
                        size_t space = argsStr.find(' ', pos);
                        if (space == std::string::npos) space = argsStr.length();
                        cmd.args[key] = argsStr.substr(pos, space - pos);
                        pos = space;
                    }
                }
            }
            if (cmd.type == "text") {
                lastTextArgs = cmd.args;
                lastTextArgs.erase("voice");
                lastTextArgs.erase("content");
                if (cmd.args.count("content") == 0) continue;
            }
            script.push_back(cmd);
        }
        else {
            PrismatiX::Models::VNCommand cmd;
            cmd.type = "text";
            cmd.args = lastTextArgs;
            cmd.args["content"] = clean;
            script.push_back(cmd);
        }
    }
    return script;
}

SDL_Color ParseColor(const std::string& colorStr, SDL_Color defaultColor) {
    if (colorStr.empty()) return defaultColor;
    std::stringstream ss(colorStr);
    std::string r, g, b;
    if (std::getline(ss, r, ',') && std::getline(ss, g, ',') && std::getline(ss, b, ',')) {
        try {
            return { (Uint8)std::stoi(r), (Uint8)std::stoi(g), (Uint8)std::stoi(b), 255 };
        } catch (...) {
            return defaultColor;
        }
    }
    return defaultColor;
}

} // namespace PrismatiX::VN
