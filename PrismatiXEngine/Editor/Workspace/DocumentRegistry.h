#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Uuid.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace px::editor {

struct ExportArtifact {
    std::string label;
    std::filesystem::path relativePath;
    std::string content;
};

enum class EditorWorkspace { UI, Story, Flow, Script };
enum class DocumentType { UIScene, Scenario, Lua, Resource, Unknown };

struct DocumentId {
    Uuid assetGuid;
    std::filesystem::path canonicalPath;

    auto operator<=>(const DocumentId&) const = default;
};

struct DocumentViewportState {
    float zoom = 1.0f;
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    bool fitToViewport = true;
    bool leftPanelVisible = true;
    bool rightPanelVisible = true;
    bool bottomPanelVisible = true;
    float leftPanelWidth = 260.0f;
    float rightPanelWidth = 340.0f;
    float bottomPanelHeight = 240.0f;
};

struct DocumentSession {
    DocumentId id;
    std::string label;
    DocumentType type = DocumentType::Unknown;
    EditorWorkspace workspace = EditorWorkspace::UI;
    bool dirty = false;
    bool pinned = false;
    std::uint64_t dirtyRevision = 0;
    std::uint64_t savedRevision = 0;
    std::uint64_t recentSequence = 0;
    std::filesystem::file_time_type diskVersion{};
    std::filesystem::path recoveryPath;
    std::string selection;
    DocumentViewportState viewport;
};

enum class ExternalDocumentState { Unchanged, ReloadSafe, LocalConflict, Missing };

// Owns editor-level document identity and session state. Concrete editors keep
// their parsed document objects, while this manager is the single authority for
// tabs, canonical path de-duplication, dirty/pinned state and session restore.
class DocumentManager {
public:
    using DocumentInfo = DocumentSession;

    void Clear();
    [[nodiscard]] std::size_t Open(DocumentSession session);
    [[nodiscard]] bool Close(const DocumentId& id);
    [[nodiscard]] bool Close(const std::filesystem::path& path);
    [[nodiscard]] bool Activate(const DocumentId& id);
    [[nodiscard]] bool Activate(const std::filesystem::path& path);
    [[nodiscard]] bool Relocate(const std::filesystem::path& oldPath,
                                const std::filesystem::path& newPath,
                                const Uuid& guid = {});

    void Upsert(DocumentSession session) { (void)Open(std::move(session)); }
    void SetDirty(const std::filesystem::path& path, bool dirty,
                  std::uint64_t revision = 0);
    void SetPinned(const std::filesystem::path& path, bool pinned);
    void SetViewport(const std::filesystem::path& path, DocumentViewportState state);
    void MarkSaved(const std::filesystem::path& path, std::uint64_t revision);
    void AcknowledgeDiskVersion(const std::filesystem::path& path);

    [[nodiscard]] DocumentSession* Find(const DocumentId& id);
    [[nodiscard]] const DocumentSession* Find(const DocumentId& id) const;
    [[nodiscard]] DocumentSession* Find(const std::filesystem::path& path);
    [[nodiscard]] const DocumentSession* Find(const std::filesystem::path& path) const;
    [[nodiscard]] DocumentSession* Active();
    [[nodiscard]] const DocumentSession* Active() const;
    [[nodiscard]] std::optional<std::size_t> ActiveIndex() const { return m_active; }
    [[nodiscard]] const std::vector<DocumentSession>& Documents() const { return m_documents; }
    [[nodiscard]] std::vector<std::size_t> RecentlyUsed() const;
    [[nodiscard]] ExternalDocumentState CheckExternalState(const DocumentSession& session) const;

    [[nodiscard]] Status SaveSession(const std::filesystem::path& path) const;
    [[nodiscard]] Status RestoreSession(const std::filesystem::path& path);

    [[nodiscard]] static std::filesystem::path Canonical(const std::filesystem::path& path);
    [[nodiscard]] static DocumentType TypeFromPath(const std::filesystem::path& path);
    [[nodiscard]] static EditorWorkspace WorkspaceFor(DocumentType type);

private:
    [[nodiscard]] std::optional<std::size_t> IndexOf(const DocumentId& id) const;
    [[nodiscard]] std::optional<std::size_t> IndexOf(const std::filesystem::path& path) const;
    void Touch(std::size_t index);

    std::vector<DocumentSession> m_documents;
    std::optional<std::size_t> m_active;
    std::uint64_t m_sequence = 0;
};

}  // namespace px::editor
