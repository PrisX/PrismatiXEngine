#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>

struct VNCommand {
    std::string type;
    std::map<std::string, std::string> args;
};

class ScriptManager {
public:
    static std::vector<VNCommand> ParseFile(const std::string& filePath) {
        std::vector<VNCommand> script;
        std::ifstream file(filePath);

        if (!file.is_open()) {
            std::cerr << "Can't open script file (" << filePath << "): " << std::endl;
            return script;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.substr(0, 2) == "//") continue;

            if (line.front() == '[' && line.back() == ']') {
                std::string content = line.substr(1, line.size() - 2);

                std::stringstream ss(content);
                VNCommand cmd;

                ss >> cmd.type;

                std::string token;
                while (ss >> token) {
                    size_t eqPos = token.find('=');
                    if (eqPos != std::string::npos) {
                        std::string key = token.substr(0, eqPos);
                        std::string val = token.substr(eqPos + 1);
                        cmd.args[key] = val;
                    }
                }
                script.push_back(cmd);
            }
        }
        file.close();
        return script;
    }
};