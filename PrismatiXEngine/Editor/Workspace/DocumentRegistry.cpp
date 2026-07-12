#include "Editor/Workspace/DocumentRegistry.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Resources/TypedDocument.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace px::editor {
namespace {

diag::Diagnostic SessionError(const std::filesystem::path& path, std::string message,
                              std::string details = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
        .code = "PXDOC2101", .category = "Editor.DocumentManager",
        .message = std::move(message), .details = std::move(details)};
    diagnostic.source.path = path.generic_string();
    return diagnostic;
}

std::string TypeName(DocumentType type) {
    switch (type) {
        case DocumentType::UIScene: return "UIScene";
        case DocumentType::Scenario: return "Scenario";
        case DocumentType::Lua: return "Lua";
        case DocumentType::Resource: return "Resource";
        default: return "Unknown";
    }
}

DocumentType ParseType(const std::string& value) {
    if (value == "UIScene") return DocumentType::UIScene;
    if (value == "Scenario") return DocumentType::Scenario;
    if (value == "Lua") return DocumentType::Lua;
    if (value == "Resource") return DocumentType::Resource;
    return DocumentType::Unknown;
}

std::int64_t Integer(const VariantObject& object, const char* key, std::int64_t fallback = 0) {
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (const auto* value = found->second.TryGet<std::int64_t>()) return *value;
    return fallback;
}

std::string String(const VariantObject& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return {};
    if (const auto* value = found->second.TryGet<std::string>()) return *value;
    return {};
}

bool Boolean(const VariantObject& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return false;
    if (const auto* value = found->second.TryGet<bool>()) return *value;
    return false;
}

float Number(const VariantObject& object,const char* key,float fallback=0.0f){
    const auto found=object.find(key);if(found==object.end())return fallback;
    if(const auto* value=found->second.TryGet<double>())return static_cast<float>(*value);
    if(const auto* value=found->second.TryGet<std::int64_t>())return static_cast<float>(*value);
    return fallback;
}

}  // namespace

std::filesystem::path DocumentManager::Canonical(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) canonical = std::filesystem::absolute(path, error);
    return (error ? path : canonical).lexically_normal();
}

DocumentType DocumentManager::TypeFromPath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (extension == ".pxscene" || extension == ".pxcomponent") return DocumentType::UIScene;
    if (extension == ".pxscenario") return DocumentType::Scenario;
    if (extension == ".lua") return DocumentType::Lua;
    if (extension == ".pxres" || extension == ".pxtheme" || extension == ".pxanim")
        return DocumentType::Resource;
    return DocumentType::Unknown;
}

EditorWorkspace DocumentManager::WorkspaceFor(DocumentType type) {
    if (type == DocumentType::UIScene || type == DocumentType::Resource) return EditorWorkspace::UI;
    if (type == DocumentType::Lua) return EditorWorkspace::Script;
    return EditorWorkspace::Story;
}

void DocumentManager::Clear() {
    m_documents.clear(); m_active.reset(); m_sequence = 0;
}

std::optional<std::size_t> DocumentManager::IndexOf(const std::filesystem::path& path) const {
    const auto canonical = Canonical(path);
    for (std::size_t index = 0; index < m_documents.size(); ++index)
        if (m_documents[index].id.canonicalPath == canonical) return index;
    return std::nullopt;
}

std::optional<std::size_t> DocumentManager::IndexOf(const DocumentId& id) const {
    for (std::size_t index = 0; index < m_documents.size(); ++index) {
        const auto& current = m_documents[index].id;
        if (!id.assetGuid.Empty() && current.assetGuid == id.assetGuid) return index;
        if (!id.canonicalPath.empty() && current.canonicalPath == Canonical(id.canonicalPath)) return index;
    }
    return std::nullopt;
}

void DocumentManager::Touch(std::size_t index) {
    m_documents[index].recentSequence = ++m_sequence;
    m_active = index;
}

std::size_t DocumentManager::Open(DocumentSession session) {
    session.id.canonicalPath = Canonical(session.id.canonicalPath);
    if (session.type == DocumentType::Unknown) session.type = TypeFromPath(session.id.canonicalPath);
    if (session.label.empty()) session.label = session.id.canonicalPath.filename().string();
    session.workspace = WorkspaceFor(session.type);
    std::error_code error;
    session.diskVersion = std::filesystem::last_write_time(session.id.canonicalPath, error);
    if (const auto found = IndexOf(session.id)) {
        auto& existing = m_documents[*found];
        const bool dirty = existing.dirty;
        const bool pinned = existing.pinned;
        const auto dirtyRevision = existing.dirtyRevision;
        const auto savedRevision = existing.savedRevision;
        const auto viewport = existing.viewport;
        const auto selection = existing.selection;
        const auto recoveryPath = existing.recoveryPath;
        existing = std::move(session);
        existing.dirty = dirty;
        existing.pinned = pinned;
        existing.dirtyRevision = dirtyRevision;
        existing.savedRevision = savedRevision;
        existing.viewport = viewport;
        existing.selection = selection;
        existing.recoveryPath = recoveryPath;
        Touch(*found);
        return *found;
    }
    m_documents.push_back(std::move(session));
    Touch(m_documents.size() - 1);
    return m_documents.size() - 1;
}

bool DocumentManager::Close(const DocumentId& id) {
    const auto index = IndexOf(id); if (!index) return false;
    m_documents.erase(m_documents.begin() + static_cast<std::ptrdiff_t>(*index));
    if (m_documents.empty()) m_active.reset();
    else if (m_active) m_active = std::min(*m_active, m_documents.size() - 1);
    return true;
}

bool DocumentManager::Close(const std::filesystem::path& path) {
    const auto found = Find(path); return found ? Close(found->id) : false;
}

bool DocumentManager::Activate(const DocumentId& id) {
    const auto index = IndexOf(id); if (!index) return false; Touch(*index); return true;
}

bool DocumentManager::Activate(const std::filesystem::path& path) {
    const auto index = IndexOf(path); if (!index) return false; Touch(*index); return true;
}

bool DocumentManager::Relocate(const std::filesystem::path& oldPath,
                               const std::filesystem::path& newPath, const Uuid& guid) {
    const auto index = IndexOf(oldPath); if (!index) return false;
    auto& session = m_documents[*index];
    session.id.canonicalPath = Canonical(newPath);
    if (!guid.Empty()) session.id.assetGuid = guid;
    session.label = newPath.filename().string();
    std::error_code error;
    session.diskVersion = std::filesystem::last_write_time(newPath, error);
    return true;
}

DocumentSession* DocumentManager::Find(const DocumentId& id) {
    const auto index = IndexOf(id); return index ? &m_documents[*index] : nullptr;
}
const DocumentSession* DocumentManager::Find(const DocumentId& id) const {
    const auto index = IndexOf(id); return index ? &m_documents[*index] : nullptr;
}
DocumentSession* DocumentManager::Find(const std::filesystem::path& path) {
    const auto index = IndexOf(path); return index ? &m_documents[*index] : nullptr;
}
const DocumentSession* DocumentManager::Find(const std::filesystem::path& path) const {
    const auto index = IndexOf(path); return index ? &m_documents[*index] : nullptr;
}
DocumentSession* DocumentManager::Active() {
    return m_active && *m_active < m_documents.size() ? &m_documents[*m_active] : nullptr;
}
const DocumentSession* DocumentManager::Active() const {
    return m_active && *m_active < m_documents.size() ? &m_documents[*m_active] : nullptr;
}

void DocumentManager::SetDirty(const std::filesystem::path& path, bool dirty,
                               std::uint64_t revision) {
    if (auto* session = Find(path)) {
        session->dirty = dirty;
        session->dirtyRevision = revision ? revision : session->dirtyRevision + (dirty ? 1 : 0);
    }
}
void DocumentManager::SetPinned(const std::filesystem::path& path, bool pinned) {
    if (auto* session = Find(path)) session->pinned = pinned;
}
void DocumentManager::SetViewport(const std::filesystem::path& path, DocumentViewportState state) {
    if (auto* session = Find(path)) session->viewport = state;
}
void DocumentManager::MarkSaved(const std::filesystem::path& path, std::uint64_t revision) {
    if (auto* session = Find(path)) {
        session->savedRevision = revision; session->dirtyRevision = revision; session->dirty = false;
        std::error_code error; session->diskVersion = std::filesystem::last_write_time(path, error);
    }
}

void DocumentManager::AcknowledgeDiskVersion(const std::filesystem::path& path) {
    if (auto* session = Find(path)) {
        std::error_code error;
        session->diskVersion = std::filesystem::last_write_time(path, error);
    }
}

std::vector<std::size_t> DocumentManager::RecentlyUsed() const {
    std::vector<std::size_t> result(m_documents.size());
    for (std::size_t index = 0; index < result.size(); ++index) result[index] = index;
    std::sort(result.begin(), result.end(), [this](std::size_t left, std::size_t right) {
        return m_documents[left].recentSequence > m_documents[right].recentSequence;
    });
    return result;
}

ExternalDocumentState DocumentManager::CheckExternalState(const DocumentSession& session) const {
    std::error_code error;
    if (!std::filesystem::exists(session.id.canonicalPath, error)) return ExternalDocumentState::Missing;
    const auto current = std::filesystem::last_write_time(session.id.canonicalPath, error);
    if (error || current == session.diskVersion) return ExternalDocumentState::Unchanged;
    return session.dirty ? ExternalDocumentState::LocalConflict : ExternalDocumentState::ReloadSafe;
}

Status DocumentManager::SaveSession(const std::filesystem::path& path) const {
    resource::TypedDocument document;
    document.kind = resource::DocumentKind::Resource;
    document.formatVersion = resource::TypedDocument::CurrentVersion;
    document.id = Uuid::FromName("PrismatiXEditor.Session.v4");
    document.type = "EditorSession";
    document.properties["active"] = Variant(static_cast<std::int64_t>(m_active.value_or(0)));
    VariantArray sessions;
    for (const auto& session : m_documents) {
        VariantObject object;
        object["path"] = Variant(session.id.canonicalPath.generic_string());
        object["guid"] = Variant(session.id.assetGuid.ToString());
        object["label"] = Variant(session.label);
        object["type"] = Variant(TypeName(session.type));
        object["pinned"] = Variant(session.pinned);
        object["recent"] = Variant(static_cast<std::int64_t>(session.recentSequence));
        object["zoom"] = Variant(static_cast<double>(session.viewport.zoom));
        object["scrollX"] = Variant(static_cast<double>(session.viewport.scrollX));
        object["scrollY"] = Variant(static_cast<double>(session.viewport.scrollY));
        object["fitToViewport"] = Variant(session.viewport.fitToViewport);
        sessions.emplace_back(std::move(object));
    }
    document.properties["documents"] = Variant(std::move(sessions));
    return io::AtomicFile::WriteText(path, resource::WriteTypedDocument(document));
}

Status DocumentManager::RestoreSession(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return Status::Ok();
    std::ifstream input(path, std::ios::binary);
    if (!input) return Status::Fail(SessionError(path, "無法讀取 Editor 文件工作階段"));
    std::ostringstream buffer; buffer << input.rdbuf();
    auto parsed = resource::ParseTypedDocument(buffer.str(), path.generic_string());
    if (!parsed) return Status::Fail(parsed.Diagnostics());
    if (parsed.Value().type != "EditorSession" || parsed.Value().formatVersion != resource::TypedDocument::CurrentVersion)
        return Status::Fail(SessionError(path, "Editor 文件工作階段版本不相容"));
    Clear();
    const auto found = parsed.Value().properties.find("documents");
    if (found != parsed.Value().properties.end()) {
        if (const auto* array = found->second.AsArray()) {
            for (const auto& item : *array) {
                const auto* object = item.AsObject(); if (!object) continue;
                DocumentSession session;
                session.id.canonicalPath = String(*object, "path");
                if (const auto guid = Uuid::Parse(String(*object, "guid"))) session.id.assetGuid = *guid;
                session.label = String(*object, "label");
                session.type = ParseType(String(*object, "type"));
                session.pinned = Boolean(*object, "pinned");
                session.recentSequence = static_cast<std::uint64_t>(Integer(*object, "recent"));
                session.viewport.zoom=Number(*object,"zoom",1.0f);
                session.viewport.scrollX=Number(*object,"scrollX");
                session.viewport.scrollY=Number(*object,"scrollY");
                if (const auto it = object->find("fitToViewport"); it != object->end())
                    if (const auto* value = it->second.TryGet<bool>()) session.viewport.fitToViewport = *value;
                if (std::filesystem::exists(session.id.canonicalPath)) (void)Open(std::move(session));
            }
        }
    }
    const auto active = parsed.Value().properties.find("active");
    if (active != parsed.Value().properties.end())
        if (const auto* index = active->second.TryGet<std::int64_t>(); index && *index >= 0 && static_cast<std::size_t>(*index) < m_documents.size())
            m_active = static_cast<std::size_t>(*index);
    return Status::Ok();
}

}  // namespace px::editor
