#pragma once

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace px::editor {

using Json = nlohmann::json;
using LogSink = std::function<void(const std::string&)>;

struct ProjectManifest {
    std::string name = "PrismatiX Project";
    int version = 2;
    int gameWidth = 1280;
    int gameHeight = 720;
    std::string startUi = "Data/UI/title.pxui";
    std::string startScript = "test_scene.pds";
    std::string theme = "PrismatiX Dark";
    std::vector<std::string> assetRoots{ "Data" };
    bool encrypt = true;
    std::string encryptKey = "prismatix";
    bool singleFile = false;
};

struct ProjectContext {
    std::filesystem::path root;
    ProjectManifest manifest;

    [[nodiscard]] bool IsOpen() const { return !root.empty(); }
    [[nodiscard]] std::filesystem::path DataRoot() const { return root / "Data"; }
    [[nodiscard]] std::filesystem::path ExportRoot() const { return root / "Export"; }
    [[nodiscard]] std::filesystem::path ManifestPath() const { return root / "project.prismatix.json"; }
};

struct AssetRecord {
    std::string runtimePath;
    std::filesystem::path absolutePath;
    std::string type;
    std::uintmax_t size = 0;
};

class ProjectService {
public:
    explicit ProjectService(LogSink log = {}) : m_log(std::move(log)) {}

    [[nodiscard]] ProjectContext& Context() { return m_context; }
    [[nodiscard]] const ProjectContext& Context() const { return m_context; }

    bool Open(const std::filesystem::path& root);
    bool Create(const std::filesystem::path& root, const std::string& name,
                const std::filesystem::path& fontSource = {});
    bool SaveManifest() const;

private:
    void Log(const std::string& msg) const {
        if (m_log) m_log(msg);
    }
    LogSink m_log;
    ProjectContext m_context;
};

class AssetDatabase {
public:
    explicit AssetDatabase(LogSink log = {}) : m_log(std::move(log)) {}

    void Scan(const ProjectContext& context);
    [[nodiscard]] const std::vector<AssetRecord>& Assets() const { return m_assets; }
    [[nodiscard]] std::vector<AssetRecord> Filter(std::string_view text,
                                                  std::string_view type) const;
    [[nodiscard]] static std::string Classify(const std::filesystem::path& path);

private:
    void Log(const std::string& msg) const {
        if (m_log) m_log(msg);
    }
    LogSink m_log;
    std::vector<AssetRecord> m_assets;
};

struct ExportArtifact {
    std::string label;
    std::filesystem::path relativePath;
    std::string content;
};

class DocumentRegistry {
public:
    struct DocumentInfo {
        std::string label;
        std::string type;
        std::filesystem::path path;
        bool dirty = false;
    };
    void Clear() { m_documents.clear(); }
    void Upsert(DocumentInfo info);
    void SetDirty(const std::filesystem::path& path, bool dirty);
    [[nodiscard]] const std::vector<DocumentInfo>& Documents() const { return m_documents; }

private:
    std::vector<DocumentInfo> m_documents;
};

class UndoStack {
public:
    struct Command {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };
    void Record(Command cmd);
    bool Undo();
    bool Redo();
    void Clear();
    [[nodiscard]] bool CanUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool CanRedo() const { return !m_redo.empty(); }
    [[nodiscard]] std::string NextUndoLabel() const {
        return m_undo.empty() ? "" : m_undo.back().label;
    }
    [[nodiscard]] std::string NextRedoLabel() const {
        return m_redo.empty() ? "" : m_redo.back().label;
    }

private:
    std::vector<Command> m_undo;
    std::vector<Command> m_redo;
};

}
