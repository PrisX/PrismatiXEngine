#include "Engine/Progression/GlobalProfile.h"

#include <nlohmann/json.hpp>

namespace px::progress {

void GlobalProfile::MarkSeen(const std::string& key) {
    if (m_seen.insert(key).second) m_dirty = true;
}
bool GlobalProfile::HasSeen(const std::string& key) const {
    return m_seen.count(key) != 0;
}

void GlobalProfile::MarkChoiceSeen(const std::string& key) {
    if (m_choicesSeen.insert(key).second) m_dirty = true;
}
bool GlobalProfile::HasChoiceSeen(const std::string& key) const {
    return m_choicesSeen.count(key) != 0;
}

void GlobalProfile::RegisterClear(const std::string& route) {
    ++m_clearCount;
    if (!route.empty()) m_clearedRoutes.insert(route);
    m_dirty = true;
}
bool GlobalProfile::RouteCleared(const std::string& route) const {
    return m_clearedRoutes.count(route) != 0;
}

void GlobalProfile::SetVariable(const std::string& name, Variant value) {
    if (name.empty()) return;
    m_vars[name] = value.Clone();
    m_dirty = true;
}
const Variant* GlobalProfile::Variable(const std::string& name) const {
    const auto found = m_vars.find(name);
    return found == m_vars.end() ? nullptr : &found->second;
}

void GlobalProfile::UnlockScene(const std::string& id) {
    if (m_scenes.insert(id).second) m_dirty = true;
}
bool GlobalProfile::SceneUnlocked(const std::string& id) const {
    return m_scenes.count(id) != 0;
}
void GlobalProfile::UnlockCG(const std::string& id) {
    if (m_cgs.insert(id).second) m_dirty = true;
}
bool GlobalProfile::CGUnlocked(const std::string& id) const {
    return m_cgs.count(id) != 0;
}
void GlobalProfile::UnlockMusic(const std::string& id) {
    if (m_music.insert(id).second) m_dirty = true;
}
bool GlobalProfile::MusicUnlocked(const std::string& id) const {
    return m_music.count(id) != 0;
}

nlohmann::json GlobalProfile::ToJson() const {
    nlohmann::json j;
    j["format"] = "PrismatiXProfile";
    j["schemaRevision"] = 2;
    j["seen"] = m_seen;
    j["choicesSeen"] = m_choicesSeen;
    j["clearedRoutes"] = m_clearedRoutes;
    j["scenes"] = m_scenes;
    j["cgs"] = m_cgs;
    j["music"] = m_music;
    j["vars"] = nlohmann::json::object();
    for (const auto& [name, value] : m_vars) {
        if (value.Is<bool>()) j["vars"][name] = *value.TryGet<bool>();
        else if (value.Is<std::int64_t>()) j["vars"][name] = *value.TryGet<std::int64_t>();
        else if (value.Is<double>()) j["vars"][name] = *value.TryGet<double>();
        else if (value.Is<std::string>()) j["vars"][name] = *value.TryGet<std::string>();
    }
    j["clearCount"] = m_clearCount;
    return j;
}

bool GlobalProfile::ApplyJson(const nlohmann::json& j) {
    if (j.value("format", std::string{}) != "PrismatiXProfile" ||
        j.value("schemaRevision", 0) != 2) {
        return false;
    }
    auto loadSet = [&](const char* name, std::set<std::string>& dst) -> bool {
        if (!j.contains(name)) return true;
        if (!j[name].is_array() || j[name].size() > 1'000'000) return false;
        for (const auto& v : j[name]) {
            if (!v.is_string()) return false;
            dst.insert(v.get<std::string>());
        }
        return true;
    };
    std::set<std::string> seen, choicesSeen, clearedRoutes, scenes, cgs, music;
    if (!loadSet("seen", seen) || !loadSet("choicesSeen", choicesSeen) ||
        !loadSet("clearedRoutes", clearedRoutes) || !loadSet("scenes", scenes) ||
        !loadSet("cgs", cgs) || !loadSet("music", music)) return false;
    std::unordered_map<std::string, Variant> variables;
    if (j.contains("vars")) {
        if (!j["vars"].is_object() || j["vars"].size() > 100'000) return false;
        for (auto it = j["vars"].begin(); it != j["vars"].end(); ++it) {
            if (it.value().is_boolean()) variables.emplace(it.key(), Variant(it.value().get<bool>()));
            else if (it.value().is_number_integer()) variables.emplace(it.key(), Variant(it.value().get<std::int64_t>()));
            else if (it.value().is_number()) variables.emplace(it.key(), Variant(it.value().get<double>()));
            else if (it.value().is_string()) variables.emplace(it.key(), Variant(it.value().get<std::string>()));
            else return false;
        }
    }
    if (!j.value("clearCount", nlohmann::json{}).is_number_integer()) return false;
    const int clearCount = j.value("clearCount", 0);
    if (clearCount < 0) return false;
    m_seen = std::move(seen);
    m_choicesSeen = std::move(choicesSeen);
    m_clearedRoutes = std::move(clearedRoutes);
    m_scenes = std::move(scenes);
    m_cgs = std::move(cgs);
    m_music = std::move(music);
    m_vars = std::move(variables);
    m_clearCount = clearCount;
    m_dirty = false;
    return true;
}

}
