#pragma once

#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
#include "Engine/UI/UIResourceResolver.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace px::editor {

// Component editing is deliberately centralized here.  Panels and canvas tools
// never mutate a ComponentInstance's source hierarchy; they can only create an
// instance, write a validated property override, reset overrides, or detach it.
class ComponentService {
public:
    using ComponentWriter =
        std::function<Result<ResourceRefValue>(const std::filesystem::path&, const std::string&)>;
    using ComponentLoader = ui::UIDocumentLoader;

    struct OverrideInfo {
        Uuid instance;
        Uuid sourceNode;
        std::string property;
        Variant value;
    };

    void SetWriter(ComponentWriter writer) { m_writer = std::move(writer); }
    void SetLoader(ComponentLoader loader) { m_loader = std::move(loader); }

    [[nodiscard]] Status CreateFromSelection(UISceneDocument& document,
                                             DesignerCommandService& commands,
                                             const Uuid& selected,
                                             const Rect& visualRect,
                                             const std::filesystem::path& componentPath) const;
    [[nodiscard]] Status Instantiate(UISceneDocument& document, DesignerCommandService& commands,
                                     const ResourceRefValue& component,
                                     const Uuid& parent, std::size_t index,
                                     Rect offsets = {20.0f, 20.0f, 180.0f, 52.0f}) const;
    [[nodiscard]] Status SetPropertyOverride(UISceneDocument& document,
                                             DesignerCommandService& commands,
                                             const Uuid& instance,
                                             const Uuid& sourceNode,
                                             const std::string& property,
                                             const Variant& value) const;
    [[nodiscard]] Status ResetPropertyOverride(UISceneDocument& document,
                                               DesignerCommandService& commands,
                                               const Uuid& instance,
                                               const Uuid& sourceNode,
                                               const std::string& property) const;
    [[nodiscard]] Status ResetAllOverrides(UISceneDocument& document,
                                           DesignerCommandService& commands,
                                           const Uuid& instance) const;
    [[nodiscard]] Status Detach(UISceneDocument& document, DesignerCommandService& commands,
                                const Uuid& instance) const;
    [[nodiscard]] Status SetInterfaceDefinitions(UISceneDocument& document,
                                                 DesignerCommandService& commands,
                                                 std::string field, Variant definitions,
                                                 std::string label) const;
    [[nodiscard]] Status SetInstanceInterface(UISceneDocument& document,
                                              DesignerCommandService& commands,
                                              const Uuid& instance, std::string field,
                                              Variant value, std::string label) const;
    [[nodiscard]] Status AssignSlot(UISceneDocument& document,
                                    DesignerCommandService& commands, const Uuid& child,
                                    std::string slot) const;

    [[nodiscard]] std::vector<OverrideInfo> Overrides(const UISceneDocument& document,
                                                      const Uuid& instance) const;
    [[nodiscard]] std::size_t OverrideCount(const UISceneDocument& document,
                                            const Uuid& instance) const;
    [[nodiscard]] std::vector<diag::Diagnostic> Validate(const UISceneDocument& document) const;

private:
    [[nodiscard]] Result<resource::TypedDocument> Load(const ResourceRefValue& reference) const;
    [[nodiscard]] static diag::Diagnostic Problem(diag::Severity severity, std::string code,
                                                  std::string message,
                                                  const UISceneDocument& document,
                                                  const Uuid& node = {},
                                                  std::string property = {});

    ComponentWriter m_writer;
    ComponentLoader m_loader;
};

}  // namespace px::editor
