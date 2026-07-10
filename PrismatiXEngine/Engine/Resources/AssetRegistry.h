#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Uuid.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::resource {

struct AssetEntry {
    Uuid id;
    std::filesystem::path sourcePath;
    std::filesystem::path metaPath;
    std::string type;
    std::string sourceHash;
    bool includeInBuild = true;
};

class AssetRegistry {
public:
    Status Scan(const std::filesystem::path& projectRoot);
    Result<AssetEntry> RegisterAsset(const std::filesystem::path& projectRoot,
                                     const std::filesystem::path& assetPath,
                                     std::string type = {});
    Result<Uuid> ReassignIdentity(const std::filesystem::path& sourcePath);
    Status SetIncludeInBuild(const std::filesystem::path& sourcePath, bool include);

    [[nodiscard]] const AssetEntry* Find(const Uuid& id) const;
    [[nodiscard]] const AssetEntry* FindPath(const std::filesystem::path& path) const;
    [[nodiscard]] const std::vector<AssetEntry>& Entries() const { return m_entries; }
    [[nodiscard]] const std::vector<diag::Diagnostic>& Diagnostics() const { return m_diagnostics; }
    [[nodiscard]] bool Valid() const;

    [[nodiscard]] static std::filesystem::path MetaPath(const std::filesystem::path& asset);

private:
    Result<AssetEntry> LoadMeta(const std::filesystem::path& source,
                                const std::filesystem::path& meta) const;
    Status SaveMeta(const AssetEntry& entry) const;
    void RebuildIndexes();

    std::filesystem::path m_projectRoot;
    std::vector<AssetEntry> m_entries;
    std::vector<diag::Diagnostic> m_diagnostics;
    std::unordered_map<Uuid, std::size_t, UuidHash> m_byId;
    std::unordered_map<std::string, std::size_t> m_byPath;
};

}  // namespace px::resource
