#pragma once

#include <unordered_map>
#include <string>

namespace PrismatiX::Models {

struct VNCommand {
    std::string type;
    std::unordered_map<std::string, std::string> args;
};

} // namespace PrismatiX::Models
