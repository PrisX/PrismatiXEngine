#pragma once

#include "Editor/Tools/UIDesigner/DocumentChangeSet.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/UISceneLoader.h"

#include <optional>
#include <string>
#include <vector>

namespace px::ui {
class Control;
class UIContext;
}

namespace px::editor {

// Production owner of the headless-testable preview apply path. RuntimeHost
// supplies rendering and VFS services; this class owns document/view-model
// state and applies the same incremental mutations used by the Editor preview.
class UIPreviewDocumentApplier {
public:
    explicit UIPreviewDocumentApplier(ui::UIContext& context,
                                      ui::UIDocumentLoader loader = {});

    bool Load(const resource::TypedDocument& document,
              const std::string& sourcePath);

    // Returns true when the change was patched in place. A false result means
    // the production path rebuilt the live scene (or loading failed).
    bool Apply(const resource::TypedDocument& document,
               const std::string& sourcePath,
               const DocumentChangeSet& changes);

    [[nodiscard]] ui::Control* Find(const Uuid& id) const;
    [[nodiscard]] const std::optional<resource::TypedDocument>& LastDocument() const {
        return m_lastDocument;
    }

private:
    ui::UIContext& m_context;
    ui::UIDocumentLoader m_loader;
    ui::ObservableViewModel m_viewModel;
    std::vector<ui::Binding> m_bindings;
    std::optional<resource::TypedDocument> m_lastDocument;
};

}  // namespace px::editor
