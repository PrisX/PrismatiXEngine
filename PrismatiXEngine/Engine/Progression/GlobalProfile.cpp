#include "Engine/Progression/GlobalProfile.h"

#include <cmath>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>

namespace px::progress {
namespace {

constexpr std::size_t kMaxProfileDepth = 32;
constexpr std::size_t kMaxProfileNodes = 1'000'000;
constexpr std::size_t kMaxProfileEntries = 100'000;
constexpr std::size_t kMaxProfileStringBytes = 1U * 1024U * 1024U;

bool BoundedString(const std::string& value) {
    return value.size() <= kMaxProfileStringBytes;
}

std::optional<nlohmann::json> ProfileValueToJson(const Variant& value,
                                                 const std::size_t depth,
                                                 std::size_t& nodes) {
    if (depth > kMaxProfileDepth || ++nodes > kMaxProfileNodes)
        return std::nullopt;
    switch (value.Type()) {
        case VariantType::Null: return nlohmann::json(nullptr);
        case VariantType::Bool: return nlohmann::json(*value.TryGet<bool>());
        case VariantType::Integer:
            return nlohmann::json(*value.TryGet<std::int64_t>());
        case VariantType::Number: {
            const double number = *value.TryGet<double>();
            return std::isfinite(number)
                       ? std::optional<nlohmann::json>(nlohmann::json(number))
                       : std::nullopt;
        }
        case VariantType::String: {
            const auto& text = *value.TryGet<std::string>();
            return BoundedString(text)
                       ? std::optional<nlohmann::json>(nlohmann::json(text))
                       : std::nullopt;
        }
        case VariantType::Array: {
            const auto* array = value.AsArray();
            if (!array || array->size() > kMaxProfileEntries) return std::nullopt;
            nlohmann::json encoded = nlohmann::json::array();
            for (const auto& item : *array) {
                auto converted = ProfileValueToJson(item, depth + 1, nodes);
                if (!converted) return std::nullopt;
                encoded.push_back(std::move(*converted));
            }
            return encoded;
        }
        case VariantType::Object: {
            const auto* object = value.AsObject();
            if (!object || object->size() > kMaxProfileEntries) return std::nullopt;
            nlohmann::json encoded = nlohmann::json::object();
            for (const auto& [name, item] : *object) {
                if (!BoundedString(name)) return std::nullopt;
                auto converted = ProfileValueToJson(item, depth + 1, nodes);
                if (!converted) return std::nullopt;
                encoded[name] = std::move(*converted);
            }
            return encoded;
        }
        default: return std::nullopt;
    }
}

std::optional<Variant> ProfileValueFromJson(const nlohmann::json& value,
                                            const std::size_t depth,
                                            std::size_t& nodes) {
    if (depth > kMaxProfileDepth || ++nodes > kMaxProfileNodes)
        return std::nullopt;
    if (value.is_null()) return Variant{};
    if (value.is_boolean()) return Variant(value.get<bool>());
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                         (std::numeric_limits<std::int64_t>::max)()))
            return std::nullopt;
        return Variant(static_cast<std::int64_t>(number));
    }
    if (value.is_number_integer()) return Variant(value.get<std::int64_t>());
    if (value.is_number_float()) {
        const double number = value.get<double>();
        return std::isfinite(number) ? std::optional<Variant>(Variant(number))
                                     : std::nullopt;
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return BoundedString(text) ? std::optional<Variant>(Variant(text))
                                   : std::nullopt;
    }
    if (value.is_array()) {
        if (value.size() > kMaxProfileEntries) return std::nullopt;
        VariantArray decoded;
        decoded.reserve(value.size());
        for (const auto& item : value) {
            auto converted = ProfileValueFromJson(item, depth + 1, nodes);
            if (!converted) return std::nullopt;
            decoded.push_back(std::move(*converted));
        }
        return Variant(std::move(decoded));
    }
    if (!value.is_object() || value.size() > kMaxProfileEntries)
        return std::nullopt;
    VariantObject decoded;
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (!BoundedString(item.key())) return std::nullopt;
        auto converted = ProfileValueFromJson(item.value(), depth + 1, nodes);
        if (!converted) return std::nullopt;
        decoded.emplace(item.key(), std::move(*converted));
    }
    return Variant(std::move(decoded));
}

bool RevisionOne(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() == 1;
    return value.is_number_integer() && value.get<std::int64_t>() == 1;
}

}  // namespace

bool GlobalProfile::MarkSeen(const std::string& key) {
    return m_seen.insert(key).second;
}
bool GlobalProfile::HasSeen(const std::string& key) const {
    return m_seen.count(key) != 0;
}

void GlobalProfile::MarkChoiceSeen(const std::string& key) {
    m_choicesSeen.insert(key);
}
bool GlobalProfile::HasChoiceSeen(const std::string& key) const {
    return m_choicesSeen.count(key) != 0;
}

void GlobalProfile::RegisterClear(const std::string& route) {
    ++m_clearCount;
    if (!route.empty()) m_clearedRoutes.insert(route);
}
bool GlobalProfile::RouteCleared(const std::string& route) const {
    return m_clearedRoutes.count(route) != 0;
}

void GlobalProfile::SetVariable(const std::string& name, Variant value) {
    if (name.empty()) return;
    m_vars[name] = value.Clone();
}
const Variant* GlobalProfile::Variable(const std::string& name) const {
    const auto found = m_vars.find(name);
    return found == m_vars.end() ? nullptr : &found->second;
}

bool GlobalProfile::UnlockScene(const std::string& id) {
    return m_scenes.insert(id).second;
}
bool GlobalProfile::SceneUnlocked(const std::string& id) const {
    return m_scenes.count(id) != 0;
}
bool GlobalProfile::UnlockCG(const std::string& id) {
    return m_cgs.insert(id).second;
}
bool GlobalProfile::CGUnlocked(const std::string& id) const {
    return m_cgs.count(id) != 0;
}
bool GlobalProfile::UnlockMusic(const std::string& id) {
    return m_music.insert(id).second;
}
bool GlobalProfile::MusicUnlocked(const std::string& id) const {
    return m_music.count(id) != 0;
}

nlohmann::json GlobalProfile::ToJson() const {
    nlohmann::json encoded;
    return TryToJson(encoded) ? encoded : nlohmann::json(nullptr);
}

bool GlobalProfile::TryToJson(nlohmann::json& output) const {
    if (m_vars.size() > kMaxProfileEntries || m_clearCount < 0) return false;
    nlohmann::json j;
    j["format"] = "PrismatiXProfile";
    j["schemaRevision"] = 1;
    j["seen"] = m_seen;
    j["choicesSeen"] = m_choicesSeen;
    j["clearedRoutes"] = m_clearedRoutes;
    j["scenes"] = m_scenes;
    j["cgs"] = m_cgs;
    j["music"] = m_music;
    j["vars"] = nlohmann::json::object();
    std::size_t nodes = 0;
    for (const auto& [name, value] : m_vars) {
        if (name.empty() || !BoundedString(name)) return false;
        auto encoded = ProfileValueToJson(value, 0, nodes);
        if (!encoded) return false;
        j["vars"][name] = std::move(*encoded);
    }
    j["clearCount"] = m_clearCount;
    output = std::move(j);
    return true;
}

bool GlobalProfile::ApplyJson(const nlohmann::json& j) {
    try {
        if (!j.is_object()) return false;
        const auto format = j.find("format");
        const auto revision = j.find("schemaRevision");
        if (format == j.end() || !format->is_string() ||
            format->get_ref<const std::string&>() != "PrismatiXProfile" ||
            revision == j.end() || !RevisionOne(*revision))
            return false;

        auto loadSet = [&](const char* name,
                           std::set<std::string>& destination) -> bool {
            const auto found = j.find(name);
            if (found == j.end()) return true;
            if (!found->is_array() || found->size() > kMaxProfileNodes)
                return false;
            for (const auto& value : *found) {
                if (!value.is_string()) return false;
                const auto& text = value.get_ref<const std::string&>();
                if (!BoundedString(text)) return false;
                destination.insert(text);
            }
            return true;
        };

        std::set<std::string> seen, choicesSeen, clearedRoutes, scenes, cgs, music;
        if (!loadSet("seen", seen) || !loadSet("choicesSeen", choicesSeen) ||
            !loadSet("clearedRoutes", clearedRoutes) ||
            !loadSet("scenes", scenes) || !loadSet("cgs", cgs) ||
            !loadSet("music", music))
            return false;

        std::unordered_map<std::string, Variant> variables;
        const auto encodedVariables = j.find("vars");
        if (encodedVariables != j.end()) {
            if (!encodedVariables->is_object() ||
                encodedVariables->size() > kMaxProfileEntries)
                return false;
            std::size_t nodes = 0;
            for (auto item = encodedVariables->begin();
                 item != encodedVariables->end(); ++item) {
                if (item.key().empty() || !BoundedString(item.key())) return false;
                auto decoded = ProfileValueFromJson(item.value(), 0, nodes);
                if (!decoded) return false;
                variables.emplace(item.key(), std::move(*decoded));
            }
        }

        const auto encodedClearCount = j.find("clearCount");
        if (encodedClearCount == j.end() ||
            (!encodedClearCount->is_number_integer() &&
             !encodedClearCount->is_number_unsigned()))
            return false;
        std::uint64_t clearCountValue = 0;
        if (encodedClearCount->is_number_unsigned()) {
            clearCountValue = encodedClearCount->get<std::uint64_t>();
        } else {
            const auto signedValue = encodedClearCount->get<std::int64_t>();
            if (signedValue < 0) return false;
            clearCountValue = static_cast<std::uint64_t>(signedValue);
        }
        if (clearCountValue >
            static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))
            return false;

        m_seen = std::move(seen);
        m_choicesSeen = std::move(choicesSeen);
        m_clearedRoutes = std::move(clearedRoutes);
        m_scenes = std::move(scenes);
        m_cgs = std::move(cgs);
        m_music = std::move(music);
        m_vars = std::move(variables);
        m_clearCount = static_cast<int>(clearCountValue);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

}
