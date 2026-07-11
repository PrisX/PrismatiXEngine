#pragma once

#include "Engine/Core/Result.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::ui {

enum class ActionArgumentType : std::uint8_t { None, String, Integer, Boolean, Route };
struct ActionDescriptor {
    std::string id;
    std::string label;
    std::string category;
    ActionArgumentType argument = ActionArgumentType::None;
};

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
