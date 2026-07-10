#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Uuid.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::editor {

struct RecoverySnapshot {
    Uuid documentId;
    std::filesystem::path file;
    std::filesystem::path sourcePath;
    std::string baseHash;
    std::uint64_t timestamp = 0;
    std::size_t contentSize = 0;
};

class RecoveryManager {
public:
    Status BeginSession(const Uuid& projectId, const std::filesystem::path& projectPath);
    Status EndSession();
    [[nodiscard]] bool HadUncleanSession() const { return m_hadUncleanSession; }
    [[nodiscard]] bool ShouldSnapshot(const Uuid& documentId,
                                      std::chrono::steady_clock::time_point lastEdit) const;
    Status SaveSnapshot(const Uuid& documentId, const std::filesystem::path& sourcePath,
                        const std::string& baseHash, const std::string& content);
    [[nodiscard]] Result<std::vector<RecoverySnapshot>> ListSnapshots() const;
    [[nodiscard]] Result<std::string> LoadContent(const RecoverySnapshot& snapshot) const;
    Status Discard(const RecoverySnapshot& snapshot);

private:
    [[nodiscard]] static std::filesystem::path UserRecoveryRoot();
    void Prune(const Uuid& documentId);
    Uuid m_projectId;
    std::filesystem::path m_projectPath;
    std::filesystem::path m_root;
    std::filesystem::path m_sessionMarker;
    bool m_hadUncleanSession = false;
    std::unordered_map<Uuid, std::chrono::steady_clock::time_point, UuidHash> m_lastSnapshot;
};

}  // namespace px::editor
