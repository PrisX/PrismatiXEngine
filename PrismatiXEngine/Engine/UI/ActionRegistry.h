#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Actions/ActionDescriptor.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::ui {

// Compatibility facade. New code should use ActionCatalog; both APIs expose the
// same descriptor schema so older integrations cannot drift from the designer.
class ActionRegistry {
public:
    Status Register(ActionDescriptor descriptor);
    [[nodiscard]] const ActionDescriptor* Find(std::string_view id) const;
    [[nodiscard]] const std::vector<ActionDescriptor>& Descriptors() const { return m_descriptors; }
    [[nodiscard]] static const ActionRegistry& Builtins();
private:
    std::vector<ActionDescriptor> m_descriptors;
    std::unordered_map<std::string,std::size_t> m_byId;
};

}  // namespace px::ui
