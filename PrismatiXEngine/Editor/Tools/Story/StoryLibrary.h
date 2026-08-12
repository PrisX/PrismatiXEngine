#pragma once

#include "Editor/Project/ProjectTypes.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/VN/GameCatalog.h"

#include <functional>
#include <string>

namespace px::editor {

class StoryLibrary {
public:
    using ResourceResolver = std::function<std::optional<ResourceRefValue>(const std::string&)>;

    Status Open(const ProjectContext* project, const resource::AssetRegistry* assets,
                ResourceResolver resolver);
    void Close();
    void Render();
    bool Save();

    [[nodiscard]] bool Loaded() const { return m_loaded; }
    [[nodiscard]] bool Dirty() const { return m_dirty; }
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }
    [[nodiscard]] const vn::GameCatalog& Catalog() const { return m_catalog; }

private:
    resource::NodeRecord* SelectedCharacter();
    resource::NodeRecord* SelectedExpression();
    const resource::NodeRecord* SelectedCharacter() const;
    std::vector<resource::NodeRecord*> Expressions(const Uuid& character);
    void MarkChanged();
    void RebuildCatalog();
    void AddCharacter();
    void AddExpression(const std::string& runtimePath);
    void RemoveSelectedExpression();
    void RemoveSelectedCharacter();
    void RenderCharacterList();
    void RenderCharacterEditor();
    void RenderAssetPicker();
    void OpenAssetPicker(bool replace);
    [[nodiscard]] std::string UniqueCharacterId() const;
    [[nodiscard]] std::string UniqueExpressionId(const Uuid& parent,
                                                 std::string base) const;
    [[nodiscard]] std::string CharacterIdError(const resource::NodeRecord& character) const;

    const ProjectContext* m_project = nullptr;
    const resource::AssetRegistry* m_assetRegistry = nullptr;
    ResourceResolver m_resolver;
    resource::TypedDocument m_document;
    vn::GameCatalog m_catalog;
    Uuid m_selectedCharacter;
    Uuid m_selectedExpression;
    bool m_loaded = false;
    bool m_dirty = false;
    std::uint64_t m_revision = 0;
    std::string m_search;
    std::string m_assetSearch;
    bool m_assetPickerRequested = false;
    bool m_assetPickerReplace = false;
};

}  // namespace px::editor
