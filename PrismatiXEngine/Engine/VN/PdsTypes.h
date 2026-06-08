#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::vn {


struct Arg {
    std::string key;
    std::string value;
};

struct Command {
    std::string type;
    std::vector<Arg> args;
    int line = 0;

    [[nodiscard]] const std::string* Find(std::string_view key) const {
        for (const Arg& a : args) {
            if (a.key == key) {
                return &a.value;
            }
        }
        return nullptr;
    }
    [[nodiscard]] std::string Get(std::string_view key, std::string fallback = "") const {
        const std::string* v = Find(key);
        return v ? *v : fallback;
    }
    [[nodiscard]] bool Has(std::string_view key) const { return Find(key) != nullptr; }
};

struct NodeMeta {
    int commandIndex = -1;
    std::unordered_map<std::string, std::string> fields;
};

struct LinkMeta {
    std::unordered_map<std::string, std::string> fields;
};

struct ParsedScript {
    std::vector<Command> commands;
    std::vector<NodeMeta> nodeMeta;
    std::vector<LinkMeta> linkMeta;
    std::vector<std::string> errors;
};

}
