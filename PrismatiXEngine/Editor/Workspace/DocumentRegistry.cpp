#include "Editor/Workspace/DocumentRegistry.h"

namespace px::editor {

void DocumentRegistry::Upsert(DocumentInfo info) {
    for (DocumentInfo& d : m_documents) {
        if (d.path == info.path) {
            d = std::move(info);
            return;
        }
    }
    m_documents.push_back(std::move(info));
}

void DocumentRegistry::SetDirty(const std::filesystem::path& path, bool dirty) {
    for (DocumentInfo& d : m_documents) {
        if (d.path == path) {
            d.dirty = dirty;
            return;
        }
    }
}

}
