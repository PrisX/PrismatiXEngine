#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace px::editor {

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

}
