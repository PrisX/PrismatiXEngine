#include "Engine/Progression/GlobalProfile.h"

#include "Engine/Progression/Persist.h"

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

void GlobalProfile::SetPersistentVar(const std::string& name, int value) {
    m_vars[name] = value;
    m_dirty = true;
}
int GlobalProfile::PersistentVar(const std::string& name) const {
    auto it = m_vars.find(name);
    return it != m_vars.end() ? it->second : 0;
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

bool GlobalProfile::Load(const std::string& path, const crypto::Key* key) {
    auto json = LoadJson(path, key);
    if (!json) {
        return false;
    }
    const Json& j = *json;
    if (j.value("format", std::string{}) != "PrismatiXProfile" ||
        j.value("schemaRevision", 0) != 1) {
        return false;
    }
    auto loadSet = [&](const char* name, std::set<std::string>& dst) {
        dst.clear();
        if (j.contains(name)) {
            for (const auto& v : j[name]) dst.insert(v.get<std::string>());
        }
    };
    loadSet("seen", m_seen);
    loadSet("choicesSeen", m_choicesSeen);
    loadSet("clearedRoutes", m_clearedRoutes);
    loadSet("scenes", m_scenes);
    loadSet("cgs", m_cgs);
    loadSet("music", m_music);
    m_vars.clear();
    if (j.contains("vars")) {
        for (auto it = j["vars"].begin(); it != j["vars"].end(); ++it) {
            m_vars[it.key()] = it.value().get<int>();
        }
    }
    m_clearCount = j.value("clearCount", 0);
    m_dirty = false;
    return true;
}

bool GlobalProfile::Save(const std::string& path, const crypto::Key* key) const {
    Json j;
    j["format"] = "PrismatiXProfile";
    j["schemaRevision"] = 1;
    j["seen"] = m_seen;
    j["choicesSeen"] = m_choicesSeen;
    j["clearedRoutes"] = m_clearedRoutes;
    j["scenes"] = m_scenes;
    j["cgs"] = m_cgs;
    j["music"] = m_music;
    j["vars"] = m_vars;
    j["clearCount"] = m_clearCount;
    return SaveJson(path, j, key);
}

}
