#pragma once

#include <map>
#include <string>

namespace PrismatiX {
namespace Models {

struct VNCommand {
    std::string type;
    std::map<std::string, std::string> args;
};

}  // namespace Models
}  // namespace PrismatiX
