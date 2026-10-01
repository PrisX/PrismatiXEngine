#pragma once

#include "Engine/Core/Variant.h"
#include "Engine/IO/VFS.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace px::player {

// Player operations `player.confirmations` may ask before.
inline constexpr std::string_view kConfirmableOperations[] = {
    "quickSave", "quickLoad", "save", "overwrite", "load", "returnToTitle",
    "exit", "backlogJump", "previousChoice", "nextChoice", "resetSettings"};

inline bool IsConfirmableOperation(const std::string_view operation) {
    return std::ranges::find(kConfirmableOperations, operation) !=
           std::end(kConfirmableOperations);
}

inline std::optional<std::string> SafeVfsRelativePath(const std::string& value) {
    return io::VFS::NormalizeVirtualPath(value);
}

inline std::optional<Variant> JsonVariant(const nlohmann::json& value, const int depth = 0) {
    if (depth > 32) return std::nullopt;
    if (value.is_null()) return Variant{};
    if (value.is_boolean()) return Variant(value.get<bool>());
    if (value.is_number_integer()) return Variant(value.get<std::int64_t>());
    if (value.is_number()) return Variant(value.get<double>());
    if (value.is_string()) return Variant(value.get<std::string>());
    if (value.is_array()) {
        VariantArray result;
        for (const auto& item : value) {
            auto converted = JsonVariant(item, depth + 1);
            if (!converted) return std::nullopt;
            result.push_back(std::move(*converted));
        }
        return Variant(std::move(result));
    }
    if (value.is_object()) {
        VariantObject result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            auto converted = JsonVariant(item.value(), depth + 1);
            if (!converted) return std::nullopt;
            result.emplace(item.key(), std::move(*converted));
        }
        return Variant(std::move(result));
    }
    return std::nullopt;
}

}  // namespace px::player
