#include "Engine/Progression/ProfileRules.h"

#include <algorithm>
#include <set>

namespace px::progress {

namespace {
constexpr int kMaximumDepth = 16;
constexpr std::size_t kMaximumChildren = 64;
}  // namespace

std::optional<ProfileCondition> ParseProfileCondition(const nlohmann::json& value,
                                                      const int depth) {
    if (depth > kMaximumDepth) return std::nullopt;
    if (value.is_string()) {
        const auto& flag = value.get_ref<const std::string&>();
        if (flag.empty() || flag.size() > 256) return std::nullopt;
        return ProfileCondition{ProfileCondition::Kind::Flag, flag, {}};
    }
    if (!value.is_object() || value.size() != 1) return std::nullopt;
    // Not `*value.items().begin()`: that dereferences a temporary proxy, and
    // the bindings dangle (it crashed under the NDK's libc++).
    const auto entry = value.begin();
    const std::string& name = entry.key();
    const nlohmann::json& operand = entry.value();
    if (name == "not") {
        auto child = ParseProfileCondition(operand, depth + 1);
        if (!child) return std::nullopt;
        return ProfileCondition{ProfileCondition::Kind::Not, {}, {std::move(*child)}};
    }
    if ((name != "all" && name != "any") || !operand.is_array() || operand.empty() ||
        operand.size() > kMaximumChildren)
        return std::nullopt;
    ProfileCondition condition{name == "all" ? ProfileCondition::Kind::All
                                             : ProfileCondition::Kind::Any, {}, {}};
    for (const auto& item : operand) {
        auto child = ParseProfileCondition(item, depth + 1);
        if (!child) return std::nullopt;
        condition.children.push_back(std::move(*child));
    }
    return condition;
}

bool EvaluateProfileCondition(const ProfileCondition& condition,
                              const ProfileFlagReader& flags) {
    switch (condition.kind) {
    case ProfileCondition::Kind::Flag: {
        const Variant* value = flags(condition.flag);
        if (!value) return false;
        if (const auto* integer = value->TryGet<std::int64_t>()) return *integer == 1;
        if (const auto* boolean = value->TryGet<bool>()) return *boolean;
        return false;
    }
    case ProfileCondition::Kind::All:
        return std::ranges::all_of(condition.children, [&](const ProfileCondition& child) {
            return EvaluateProfileCondition(child, flags);
        });
    case ProfileCondition::Kind::Any:
        return std::ranges::any_of(condition.children, [&](const ProfileCondition& child) {
            return EvaluateProfileCondition(child, flags);
        });
    case ProfileCondition::Kind::Not:
        return !EvaluateProfileCondition(condition.children.front(), flags);
    }
    return false;
}

std::vector<std::int64_t> ProfileUiDatum::Values() const {
    std::set<std::int64_t> values{fallback};
    for (const auto& item : cases) values.insert(item.value);
    return {values.begin(), values.end()};
}

std::int64_t ProfileUiDatum::Evaluate(const ProfileFlagReader& flags) const {
    if (condition) return EvaluateProfileCondition(*condition, flags) ? 1 : 0;
    for (const auto& item : cases)
        if (EvaluateProfileCondition(item.when, flags)) return item.value;
    return fallback;
}

std::optional<ProfileUiDatum> ParseProfileUiDatum(std::string key, const nlohmann::json& value) {
    ProfileUiDatum datum;
    datum.key = std::move(key);
    if (!value.is_object() || !value.contains("cases")) {
        datum.condition = ParseProfileCondition(value);
        if (!datum.condition) return std::nullopt;
        return datum;
    }
    const auto& cases = value["cases"];
    if (!cases.is_array() || cases.empty() || cases.size() > kMaximumChildren) return std::nullopt;
    for (const auto& [name, field] : value.items())
        if (name != "cases" && name != "default") return std::nullopt;
    if (const auto fallback = value.find("default"); fallback != value.end()) {
        if (!fallback->is_number_integer()) return std::nullopt;
        datum.fallback = fallback->get<std::int64_t>();
    }
    for (const auto& item : cases) {
        if (!item.is_object() || item.size() != 2 || !item.contains("when") ||
            !item.contains("value") || !item["value"].is_number_integer())
            return std::nullopt;
        auto when = ParseProfileCondition(item["when"]);
        if (!when) return std::nullopt;
        datum.cases.push_back({std::move(*when), item["value"].get<std::int64_t>()});
    }
    return datum;
}

const std::string* FirstMatchingChoice(const std::vector<ProfileChoice>& choices,
                                       const ProfileFlagReader& flags) {
    for (const auto& choice : choices)
        if (!choice.when || EvaluateProfileCondition(*choice.when, flags)) return &choice.value;
    return nullptr;
}

}  // namespace px::progress
