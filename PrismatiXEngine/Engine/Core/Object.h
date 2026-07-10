#pragma once

#include "Engine/Core/Uuid.h"

#include <string_view>

namespace px {

class Object {
public:
    Object() : m_id(Uuid::Random()) {}
    virtual ~Object() = default;

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;

    [[nodiscard]] const Uuid& Id() const { return m_id; }
    void SetId(Uuid id) { m_id = id; }
    [[nodiscard]] virtual std::string_view TypeName() const { return "Object"; }

private:
    Uuid m_id;
};

}  // namespace px
