#pragma once

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace px::editor {

struct AssetImportSettings {
    bool includeInBuild = true;
    std::string note;
};

[[nodiscard]] inline std::filesystem::path AssetMetaPath(const std::filesystem::path& asset) {
    return std::filesystem::path(asset.string() + ".meta");
}

[[nodiscard]] inline AssetImportSettings LoadAssetMeta(const std::filesystem::path& asset) {
    AssetImportSettings s;
    std::ifstream in(AssetMetaPath(asset));
    if (!in) return s;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) return s;
    s.includeInBuild = j.value("includeInBuild", true);
    s.note = j.value("note", std::string{});
    return s;
}

inline void SaveAssetMeta(const std::filesystem::path& asset, const AssetImportSettings& s) {
    nlohmann::json j;
    j["includeInBuild"] = s.includeInBuild;
    j["note"] = s.note;
    std::ofstream out(AssetMetaPath(asset));
    out << j.dump(2);
}

}
