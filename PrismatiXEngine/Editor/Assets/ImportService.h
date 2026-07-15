#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/AssetRegistry.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace px::editor {

enum class ImportConflictPolicy { UseExisting, KeepBoth, Replace, Skip };
enum class ImportState { Idle, Staging, Ready, Committing, Completed, Failed, Cancelled };

struct ImportSource {
    std::filesystem::path path;
    std::filesystem::path relativePath;
};

struct ImportCandidate {
    ImportSource source;
    std::filesystem::path requestedTarget;
    std::filesystem::path target;
    std::string type;
    std::uintmax_t size = 0;
    bool enabled = true;
    bool includeInBuild = true;
    bool identical = false;
    ImportConflictPolicy policy = ImportConflictPolicy::KeepBoth;
};

struct ImportPlan {
    std::filesystem::path projectRoot;
    std::filesystem::path destination;
    bool autoOrganize = false;
    bool preserveFolders = true;
    bool preserveIdentity = false;
    std::vector<ImportCandidate> items;
};

struct ImportProgress {
    ImportState state = ImportState::Idle;
    std::size_t currentItem = 0;
    std::size_t totalItems = 0;
    std::uintmax_t copiedBytes = 0;
    std::uintmax_t totalBytes = 0;
    std::string currentFile;
    std::string message;
};

struct ImportCommitRecord {
    std::filesystem::path target;
    std::filesystem::path meta;
    std::filesystem::path originalBackup;
    std::filesystem::path importedBackup;
    std::filesystem::path metaBackup;
    bool replaced = false;
};

class ImportService {
public:
    ImportService() = default;
    ~ImportService();
    ImportService(const ImportService&) = delete;
    ImportService& operator=(const ImportService&) = delete;

    [[nodiscard]] Result<ImportPlan> Prepare(const std::filesystem::path& projectRoot,
                                             const std::filesystem::path& destination,
                                             const std::vector<ImportSource>& sources,
                                             bool autoOrganize,
                                             bool preserveFolders) const;
    void RecalculateTargets(ImportPlan& plan) const;
    Status Start(ImportPlan plan);
    // Deterministic synchronization boundary for command-line tools and tests.
    // Interactive callers should continue polling Progress() without blocking.
    [[nodiscard]] ImportProgress WaitForStaging();
    void Cancel();
    [[nodiscard]] ImportProgress Progress() const;
    Status Commit(resource::AssetRegistry& registry);
    void Reset();

    [[nodiscard]] const ImportPlan& Plan() const { return m_plan; }
    [[nodiscard]] ImportPlan& Plan() { return m_plan; }
    [[nodiscard]] const std::vector<std::filesystem::path>& CommittedPaths() const {
        return m_committedPaths;
    }
    [[nodiscard]] const std::vector<ImportCommitRecord>& CommitRecords() const {
        return m_commitRecords;
    }
    [[nodiscard]] const std::filesystem::path& UndoRoot() const { return m_undoRoot; }

private:
    [[nodiscard]] Result<ImportPlan> PrepareImpl(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& destination,
        const std::vector<ImportSource>& sources,
        bool autoOrganize, bool preserveFolders) const;
    void Stage(std::stop_token stopToken) noexcept;
    [[nodiscard]] Status CommitImpl(resource::AssetRegistry& registry);
    void SetFailure(std::string message);
    [[nodiscard]] static std::string DetectType(const std::filesystem::path& path);
    [[nodiscard]] static std::filesystem::path UniqueTarget(const std::filesystem::path& path);
    [[nodiscard]] static bool SameContent(const std::filesystem::path& a,
                                          const std::filesystem::path& b);

    mutable std::mutex m_mutex;
    ImportPlan m_plan;
    ImportProgress m_progress;
    std::filesystem::path m_stagingRoot;
    std::filesystem::path m_journalPath;
    std::vector<std::filesystem::path> m_stagedFiles;
    std::vector<std::filesystem::path> m_committedPaths;
    std::vector<ImportCommitRecord> m_commitRecords;
    std::filesystem::path m_undoRoot;
    std::jthread m_worker;
    std::atomic_bool m_cancel{false};
};

}  // namespace px::editor
