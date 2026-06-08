#pragma once

#include "Engine/VN/PdsTypes.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace px::vn {

struct Program {
    std::vector<Command> code;
    std::unordered_map<std::string, int> labels;
    std::vector<int> branch;
    std::vector<std::string> errors;

    [[nodiscard]] int LabelIndex(const std::string& name) const {
        std::string key = (!name.empty() && name[0] == '*') ? name.substr(1) : name;
        auto it = labels.find(key);
        return it != labels.end() ? it->second : -1;
    }
};

[[nodiscard]] Program Compile(const ParsedScript& parsed);

}
