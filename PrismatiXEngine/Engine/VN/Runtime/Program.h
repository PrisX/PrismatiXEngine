#pragma once

#include "Engine/VN/Commands/Command.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace px::vn {

struct Program {
    std::string documentId;
    std::vector<Command> code;
    std::unordered_map<std::string, int> labels;
    std::vector<int> branch;
    std::vector<std::string> errors;

    [[nodiscard]] int LabelIndex(const std::string& name) const {
        const std::string key = !name.empty() && name.front() == '*' ? name.substr(1) : name;
        const auto found = labels.find(key);
        return found == labels.end() ? -1 : found->second;
    }
};

[[nodiscard]] Program CompileProgram(std::vector<Command> commands);

}  // namespace px::vn
