#pragma once

#include "Engine/Core/TypeRegistry.h"

#include <functional>
#include <string_view>
#include <unordered_map>

namespace px::editor {

enum class PropertyEditPhase { Begin, Update, Commit, Cancel };
struct PropertyEditRequest {
    const PropertyInfo& property;
    Variant value;
    bool mixed = false;
    PropertyEditPhase phase = PropertyEditPhase::Update;
    bool continuous = false;
};
using PropertyEditor = std::function<bool(PropertyEditRequest&)>;

class PropertyEditorRegistry {
public:
    Status Register(std::string id, PropertyEditor editor);
    [[nodiscard]] const PropertyEditor* Resolve(const PropertyInfo& property) const;
    [[nodiscard]] static PropertyEditorRegistry& Global();
private:
    std::unordered_map<std::string, PropertyEditor> m_editors;
};
}  // namespace px::editor
