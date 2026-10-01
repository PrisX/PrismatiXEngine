#pragma once

#include "Engine/Core/Variant.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace px::progress {

// A condition over the global profile's flags, written in prismatix.json as
// a flag name, or {"all": [...]}, {"any": [...]} or {"not": condition}.
// A flag holds when its profile variable is true or the integer 1.
struct ProfileCondition {
    enum class Kind : std::uint8_t { Flag, All, Any, Not };
    Kind kind = Kind::Flag;
    std::string flag;
    std::vector<ProfileCondition> children;
};

using ProfileFlagReader = std::function<const Variant*(const std::string&)>;

[[nodiscard]] std::optional<ProfileCondition> ParseProfileCondition(
    const nlohmann::json& value, int depth = 0);
[[nodiscard]] bool EvaluateProfileCondition(const ProfileCondition& condition,
                                            const ProfileFlagReader& flags);

// One `player.profileUiData` entry. A plain condition publishes a boolean at
// `key`. A {"cases": [{"when", "value"}...], "default"} entry publishes the
// value of the first case that holds as an integer at `key`, and
// `key.is<N>` booleans for every value it can take.
struct ProfileUiDatum {
    std::string key;
    std::optional<ProfileCondition> condition;
    struct Case {
        ProfileCondition when;
        std::int64_t value = 0;
    };
    std::vector<Case> cases;
    std::int64_t fallback = 0;

    [[nodiscard]] std::vector<std::int64_t> Values() const;
    [[nodiscard]] std::int64_t Evaluate(const ProfileFlagReader& flags) const;
};

[[nodiscard]] std::optional<ProfileUiDatum> ParseProfileUiDatum(
    std::string key, const nlohmann::json& value);

// `player.titleBgm` as a list of {"when"?, "path"} rules: the first rule whose
// condition holds (or that has none) picks the title music.
struct ProfileChoice {
    std::optional<ProfileCondition> when;
    std::string value;
};

[[nodiscard]] const std::string* FirstMatchingChoice(
    const std::vector<ProfileChoice>& choices, const ProfileFlagReader& flags);

}  // namespace px::progress
