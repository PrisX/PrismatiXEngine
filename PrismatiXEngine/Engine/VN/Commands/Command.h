#pragma once

#include "Engine/Core/Variant.h"

#include <string>
#include <string_view>
#include <vector>

namespace px::vn {

struct Arg {
    std::string key;
    std::string value;
};

struct Command {
    std::string type;
    std::vector<Arg> args;
    VariantObject typedArgs;
    int line = 0;
    std::string sourceId;
    std::string operationId;

    [[nodiscard]] const std::string* Find(std::string_view key) const {
        for (const auto& argument : args) if (argument.key == key) return &argument.value;
        return nullptr;
    }
    [[nodiscard]] std::string Get(std::string_view key, std::string fallback = {}) const {
        const auto* value = Find(key); return value ? *value : std::move(fallback);
    }
    [[nodiscard]] bool Has(std::string_view key) const {
        return Find(key) != nullptr || FindTyped(key) != nullptr;
    }
    [[nodiscard]] const Variant* FindTyped(std::string_view key) const {
        const auto found = typedArgs.find(std::string(key));
        return found == typedArgs.end() ? nullptr : &found->second;
    }
};

}  // namespace px::vn
