#pragma once

#include <string>
#include <vector>

namespace px::editor {

struct CustomCommandParam {
    std::string key;
    std::string label;
    std::string type = "string";
    std::string defaultValue;
    std::vector<std::string> options;
    bool required = false;
};

struct CustomCommandDef {
    std::string name;
    std::string category = "Custom";
    std::string description;
    std::vector<CustomCommandParam> params;
    std::string sourceFile;
};

}
