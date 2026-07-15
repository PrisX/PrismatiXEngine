#pragma once

#include "Editor/Tools/UIDesigner/Components/ComponentService.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::editor {

class DesignerDiagnostics {
public:
    struct ValidationContext {
        const ComponentService* components = nullptr;
        std::function<bool(std::string_view path, std::string_view expectedType)> resourceExists;
        std::function<std::vector<diag::Diagnostic>(std::string_view action,
                                                    const VariantObject& arguments,
                                                    const diag::Source& source)>
            validateAction;
        std::function<std::optional<ui::PropertyPathInfo>(std::string_view)> describeBinding;
        std::function<const ui::Formatter*(std::string_view)> findFormatter;
        std::function<ui::ChildLayoutPolicy(const Uuid&)> childPolicy;
    };

    void Refresh(const UISceneDocument& document);
    void Refresh(const UISceneDocument& document, const ValidationContext& context);
    void Clear();

    [[nodiscard]] const std::vector<diag::Diagnostic>& Items() const { return m_items; }
    [[nodiscard]] std::vector<const diag::Diagnostic*> ForNode(const Uuid& node) const;
    [[nodiscard]] std::vector<const diag::Diagnostic*> ForProperty(
        const Uuid& node, std::string_view property) const;
    [[nodiscard]] bool HasProblem(const Uuid& node) const;
    [[nodiscard]] std::size_t ErrorCount() const;
    [[nodiscard]] std::size_t WarningCount() const;

private:
    static diag::Diagnostic Make(diag::Severity severity, std::string code,
                                 std::string message, const UISceneDocument& document,
                                 const Uuid& node = {}, std::string property = {},
                                 std::string details = {});
    void Add(diag::Diagnostic diagnostic);
    void ValidateTokens(const UISceneDocument& document);

    std::vector<diag::Diagnostic> m_items;
    std::unordered_multimap<std::string, std::size_t> m_byNode;
};

}  // namespace px::editor
