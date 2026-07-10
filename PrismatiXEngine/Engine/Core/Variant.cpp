#include "Engine/Core/Variant.h"

namespace px {

VariantType Variant::Type() const {
    return static_cast<VariantType>(m_value.index());
}

const VariantArray* Variant::AsArray() const {
    if (const auto* p = std::get_if<std::shared_ptr<VariantArray>>(&m_value)) return p->get();
    return nullptr;
}
VariantArray* Variant::AsArray() {
    if (auto* p = std::get_if<std::shared_ptr<VariantArray>>(&m_value)) return p->get();
    return nullptr;
}
const VariantObject* Variant::AsObject() const {
    if (const auto* p = std::get_if<std::shared_ptr<VariantObject>>(&m_value)) return p->get();
    return nullptr;
}
VariantObject* Variant::AsObject() {
    if (auto* p = std::get_if<std::shared_ptr<VariantObject>>(&m_value)) return p->get();
    return nullptr;
}

bool Variant::operator==(const Variant& other) const {
    if (m_value.index() != other.m_value.index()) return false;
    if (const auto* a = AsArray()) {
        const auto* b = other.AsArray();
        return b && *a == *b;
    }
    if (const auto* a = AsObject()) {
        const auto* b = other.AsObject();
        return b && *a == *b;
    }
    return m_value == other.m_value;
}

Variant Variant::Clone() const {
    if(const auto* array=AsArray()){VariantArray copy;copy.reserve(array->size());for(const auto& item:*array)copy.push_back(item.Clone());return Variant(std::move(copy));}
    if(const auto* object=AsObject()){VariantObject copy;for(const auto& [key,item]:*object)copy.emplace(key,item.Clone());return Variant(std::move(copy));}
    return *this;
}

const char* ToString(VariantType type) {
    constexpr const char* names[] = { "null", "bool", "integer", "number", "string", "vec2",
                                      "rect", "color", "uuid", "resource", "array", "object" };
    const auto i = static_cast<std::size_t>(type);
    return i < std::size(names) ? names[i] : "unknown";
}

}  // namespace px
