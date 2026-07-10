#include "Engine/Progression/SaveSystem.h"

#include "Engine/Progression/Persist.h"

#include <filesystem>

namespace px::progress {

namespace {
Json ActorsToJson(const std::vector<vn::Stage::SavedActor>& actors) {
    Json arr = Json::array();
    for (const auto& a : actors) {
        arr.push_back({ { "name", a.name },
                        { "image", a.imagePath },
                        { "slot", a.slot },
                        { "ox", a.offsetX },
                        { "oy", a.offsetY },
                        { "scale", a.scale } });
    }
    return arr;
}

std::vector<vn::Stage::SavedActor> ActorsFromJson(const Json& arr) {
    std::vector<vn::Stage::SavedActor> out;
    if (!arr.is_array()) return out;
    for (const auto& j : arr) {
        out.push_back(vn::Stage::SavedActor{ j.value("name", std::string{}),
                                             j.value("image", std::string{}), j.value("slot", 2),
                                             j.value("ox", 0.0f), j.value("oy", 0.0f),
                                             j.value("scale", 1.0f) });
    }
    return out;
}

Json LayersToJson(const std::vector<vn::Stage::SavedLayer>& layers) {
    Json arr = Json::array();
    for (const auto& l : layers) {
        arr.push_back({ { "name", l.name },
                        { "image", l.imagePath },
                        { "x", l.x },
                        { "y", l.y },
                        { "scale", l.scale },
                        { "alpha", l.alpha },
                        { "z", l.z } });
    }
    return arr;
}

std::vector<vn::Stage::SavedLayer> LayersFromJson(const Json& arr) {
    std::vector<vn::Stage::SavedLayer> out;
    if (!arr.is_array()) return out;
    for (const auto& j : arr) {
        out.push_back(vn::Stage::SavedLayer{ j.value("name", std::string{}),
                                             j.value("image", std::string{}), j.value("x", 0.0f),
                                             j.value("y", 0.0f), j.value("scale", 1.0f),
                                             j.value("alpha", 255), j.value("z", 0) });
    }
    return out;
}

Json BacklogToJson(const std::vector<vn::BacklogEntry>& log) {
    Json arr = Json::array();
    for (const auto& e : log) {
        arr.push_back({ { "speaker", e.speaker },
                        { "text", e.text },
                        { "voice", e.voice },
                        { "choice", e.isChoice } });
    }
    return arr;
}

std::vector<vn::BacklogEntry> BacklogFromJson(const Json& arr) {
    std::vector<vn::BacklogEntry> out;
    if (!arr.is_array()) return out;
    for (const auto& j : arr) {
        out.push_back(vn::BacklogEntry{ j.value("speaker", std::string{}),
                                        j.value("text", std::string{}),
                                        j.value("voice", std::string{}), j.value("choice", false) });
    }
    return out;
}
}

void SaveSystem::Configure(const std::string& directory, const crypto::Key* key) {
    m_dir = directory;
    m_encrypt = key != nullptr;
    if (key) {
        m_key = *key;
    }
}

std::string SaveSystem::SlotPath(int slot) const {
    // Negative slots are reserved for the automatic save.
    const std::string file = slot < 0 ? "autosave.pxsav"
                                      : "save_" + std::to_string(slot) + ".pxsav";
    return (std::filesystem::path(m_dir) / file).string();
}

bool SaveSystem::Save(int slot, const SaveSnapshot& s) {
    Json j;
    j["version"] = 2;
    j["scriptPath"] = s.scriptPath;
    j["pc"] = s.pc;
    j["chapter"] = s.chapter;
    j["bgPath"] = s.bgPath;
    j["bgmPath"] = s.bgmPath;
    j["actors"] = ActorsToJson(s.actors);
    j["layers"] = LayersToJson(s.layers);
    j["variables"] = s.variables;
    j["backlog"] = BacklogToJson(s.backlog);
    j["nvlMode"] = s.nvlMode;
    j["nvlLines"] = BacklogToJson(s.nvlLines);
    j["timestamp"] = s.timestamp;
    j["playtimeMs"] = s.playtimeMs;
    if (!s.thumbnailPng.empty()) {
        j["thumb"] = Base64Encode(s.thumbnailPng);
    }
    return SaveJson(SlotPath(slot), j, m_encrypt ? &m_key : nullptr);
}

std::optional<SaveSnapshot> SaveSystem::Load(int slot) const {
    auto json = LoadJson(SlotPath(slot), m_encrypt ? &m_key : nullptr);
    if (!json) {
        return std::nullopt;
    }
    const Json& j = *json;
    SaveSnapshot s;
    s.scriptPath = j.value("scriptPath", std::string{});
    s.pc = j.value("pc", 0);
    s.chapter = j.value("chapter", std::string{});
    s.bgPath = j.value("bgPath", std::string{});
    s.bgmPath = j.value("bgmPath", std::string{});
    s.actors = ActorsFromJson(j.value("actors", Json::array()));
    s.layers = LayersFromJson(j.value("layers", Json::array()));
    s.nvlMode = j.value("nvlMode", false);
    s.nvlLines = BacklogFromJson(j.value("nvlLines", Json::array()));
    if (j.contains("variables")) {
        for (auto it = j["variables"].begin(); it != j["variables"].end(); ++it) {
            s.variables[it.key()] = it.value().get<int>();
        }
    }
    s.backlog = BacklogFromJson(j.value("backlog", Json::array()));
    s.timestamp = j.value("timestamp", std::uint64_t{ 0 });
    s.playtimeMs = j.value("playtimeMs", std::uint64_t{ 0 });
    if (j.contains("thumb")) {
        s.thumbnailPng = Base64Decode(j["thumb"].get<std::string>());
    }
    return s;
}

SlotInfo SaveSystem::Peek(int slot) const {
    SlotInfo info;
    auto json = LoadJson(SlotPath(slot), m_encrypt ? &m_key : nullptr);
    if (!json) {
        return info;
    }
    const Json& j = *json;
    info.exists = true;
    info.chapter = j.value("chapter", std::string{});
    info.timestamp = j.value("timestamp", std::uint64_t{ 0 });
    if (j.contains("thumb")) {
        info.thumbnailPng = Base64Decode(j["thumb"].get<std::string>());
    }
    return info;
}

std::vector<SlotInfo> SaveSystem::List(int count) const {
    std::vector<SlotInfo> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        out.push_back(Peek(i));
    }
    return out;
}

bool SaveSystem::Delete(int slot) {
    std::error_code ec;
    return std::filesystem::remove(SlotPath(slot), ec);
}

}
