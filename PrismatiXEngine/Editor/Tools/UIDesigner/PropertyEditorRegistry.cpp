#include "Editor/Tools/UIDesigner/PropertyEditorRegistry.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::editor {
Status PropertyEditorRegistry::Register(std::string id, PropertyEditor editor) {
    if (id.empty() || !editor || m_editors.contains(id)) return Status::Fail(diag::Diagnostic{
        .severity=diag::Severity::Error, .code="PXEDUI3110", .category="Editor.UIDesigner",
        .message="Invalid or duplicate property editor", .details=std::move(id)});
    m_editors.emplace(std::move(id), std::move(editor));
    return Status::Ok();
}
const PropertyEditor* PropertyEditorRegistry::Resolve(const PropertyInfo& property) const {
    std::string id = property.editor.editorId;
    if (id.empty()) {
        if (property.editor.multiline) id = "multiline";
        else if (!property.editor.enumChoices.empty()) id = "enum";
        else if (HasFlag(property.flags, PropertyFlags::ResourcePath) || !property.editor.resourceFilter.empty()) id = "resource";
        else id = "type:" + std::to_string(static_cast<int>(property.type));
    }
    const auto found = m_editors.find(id);
    return found == m_editors.end() ? nullptr : &found->second;
}
PropertyEditorRegistry& PropertyEditorRegistry::Global() {
    static PropertyEditorRegistry value;
    return value;
}
}  // namespace px::editor
