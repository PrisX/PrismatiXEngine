#pragma once

#include <string>
#include <vector>

namespace px::editor {

struct CustomCommandParam {
    std::string key;
    std::string defaultValue;
};

struct CustomCommandDef {
    std::string name;
    std::string category = "Custom";
    std::string description;
    std::vector<CustomCommandParam> params;
    std::string sourceFile;
};

}
