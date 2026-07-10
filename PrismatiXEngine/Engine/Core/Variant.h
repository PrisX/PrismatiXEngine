#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Core/Uuid.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace px {

class Variant;
using VariantArray = std::vector<Variant>;
using VariantObject = std::map<std::string, Variant>;

struct ResourceRefValue {
    Uuid id;
    std::string lastKnownPath;
    auto operator<=>(const ResourceRefValue&) const = default;
};

enum class VariantType : std::uint8_t {
    Null,
    Bool,
    Integer,
    Number,
    String,
    Vec2,
    Rect,
    Color,
    Uuid,
    ResourceRef,
    Array,
    Object,
};

class Variant {
public:
    using Storage =
        std::variant<std::monostate, bool, std::int64_t, double, std::string, Vec2, Rect, Color,
                     Uuid, ResourceRefValue, std::shared_ptr<VariantArray>,
                     std::shared_ptr<VariantObject>>;

    Variant() = default;
    Variant(bool value) : m_value(value) {}
    Variant(int value) : m_value(static_cast<std::int64_t>(value)) {}
    Variant(std::int64_t value) : m_value(value) {}
    Variant(float value) : m_value(static_cast<double>(value)) {}
    Variant(double value) : m_value(value) {}
    Variant(const char* value) : m_value(std::string(value)) {}
    Variant(std::string value) : m_value(std::move(value)) {}
    Variant(Vec2 value) : m_value(value) {}
    Variant(Rect value) : m_value(value) {}
    Variant(Color value) : m_value(value) {}
    Variant(Uuid value) : m_value(value) {}
    Variant(ResourceRefValue value) : m_value(std::move(value)) {}
    Variant(VariantArray value) : m_value(std::make_shared<VariantArray>(std::move(value))) {}
    Variant(VariantObject value) : m_value(std::make_shared<VariantObject>(std::move(value))) {}

    [[nodiscard]] VariantType Type() const;
    [[nodiscard]] const Storage& Raw() const { return m_value; }

    template <typename T>
    [[nodiscard]] bool Is() const {
        return std::holds_alternative<T>(m_value);
    }
    template <typename T>
    [[nodiscard]] const T* TryGet() const {
        return std::get_if<T>(&m_value);
    }
    template <typename T>
    [[nodiscard]] T* TryGet() {
        return std::get_if<T>(&m_value);
    }

    [[nodiscard]] const VariantArray* AsArray() const;
    [[nodiscard]] VariantArray* AsArray();
    [[nodiscard]] const VariantObject* AsObject() const;
    [[nodiscard]] VariantObject* AsObject();
    [[nodiscard]] bool operator==(const Variant& other) const;
    [[nodiscard]] Variant Clone() const;

private:
    Storage m_value;
};

[[nodiscard]] const char* ToString(VariantType type);

}  // namespace px
