#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Actions/ActionDescriptor.h"

#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::ui {

class ActionCatalog {
public:
    Status Register(ActionDescriptor descriptor);
    Status ReplaceSource(ActionOrigin origin, std::string_view sourceId,
                         std::vector<ActionDescriptor> descriptors);
    Status RemoveSource(ActionOrigin origin, std::string_view sourceId);

    [[nodiscard]] const ActionDescriptor* Find(std::string_view id) const;
    [[nodiscard]] bool Contains(std::string_view id) const { return Find(id) != nullptr; }
    [[nodiscard]] const std::vector<ActionDescriptor>& Descriptors() const { return m_descriptors; }
    [[nodiscard]] std::vector<ActionDescriptor> Search(
        std::string_view text = {}, std::string_view category = {},
        std::optional<ActionOrigin> origin = std::nullopt, bool includeUnavailable = true) const;
    [[nodiscard]] std::vector<std::string> Categories(
        std::optional<ActionOrigin> origin = std::nullopt) const;

    [[nodiscard]] Result<VariantObject> ValidateAndNormalize(
        const ActionInvocation& invocation) const;

    static ActionCatalog& Global();

private:
    static Status NormalizeAndValidateDescriptor(ActionDescriptor& descriptor);
    void Reindex();

    std::vector<ActionDescriptor> m_descriptors;
    std::unordered_map<std::string, std::size_t> m_byId;
};

}  // namespace px::ui
