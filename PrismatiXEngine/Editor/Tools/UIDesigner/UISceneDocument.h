#pragma once

#include "Editor/Workspace/EditHistory.h"
#include "Engine/Resources/TypedDocument.h"

#include <filesystem>
#include <optional>

namespace px::editor {

class UISceneDocument final : public IEditableDocument {
public:
    UISceneDocument();

    Status New(std::filesystem::path path, int width = 1280, int height = 720);
    Status Load(const std::filesystem::path& path);
    Status Save();
    [[nodiscard]] std::string Serialize() const;
    [[nodiscard]] const std::filesystem::path& Path() const { return m_path; }
    void RelocatePath(std::filesystem::path path) { m_path = std::move(path); }
    [[nodiscard]] resource::TypedDocument& Data() { return m_data; }
    [[nodiscard]] const resource::TypedDocument& Data() const { return m_data; }
    [[nodiscard]] EditHistory& History() { return m_history; }
    [[nodiscard]] const EditHistory& History() const { return m_history; }

    [[nodiscard]] Uuid DocumentId() const override { return m_data.id; }
    [[nodiscard]] Result<Variant> ReadProperty(const Uuid& target, const std::string& property) const override;
    Status WriteProperty(const Uuid& target, const std::string& property, const Variant& value) override;
    [[nodiscard]] Result<VariantObject> CaptureSubtree(const Uuid& target) const override;
    Status InsertSubtree(const Uuid& parent, std::size_t index, const VariantObject& subtree) override;
    [[nodiscard]] Result<VariantObject> RemoveSubtree(const Uuid& target) override;
    Status Reparent(const Uuid& target, const Uuid& parent, std::size_t index) override;
    Status MoveChild(const Uuid& parent, const Uuid& target, std::size_t index) override;

    [[nodiscard]] resource::NodeRecord* Find(const Uuid& id);
    [[nodiscard]] const resource::NodeRecord* Find(const Uuid& id) const;
    [[nodiscard]] std::vector<resource::NodeRecord*> Children(const Uuid& parent);
    [[nodiscard]] std::vector<const resource::NodeRecord*> Children(const Uuid& parent) const;
    [[nodiscard]] std::size_t ChildIndex(const Uuid& id) const;
    [[nodiscard]] bool IsDescendant(const Uuid& possibleDescendant, const Uuid& ancestor) const;

private:
    [[nodiscard]] Result<VariantObject> Capture(const resource::NodeRecord& record) const;
    Status DecodeAndInsert(const Uuid& parent, std::size_t index, const VariantObject& data);
    Status ValidateTree() const;
    [[nodiscard]] static diag::Diagnostic Error(std::string code, std::string message, const Uuid& node = {});

    resource::TypedDocument m_data;
    std::filesystem::path m_path;
    EditHistory m_history;
};

}  // namespace px::editor
