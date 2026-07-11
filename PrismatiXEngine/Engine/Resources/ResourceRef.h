#pragma once

#include "Engine/Core/Uuid.h"
#include "Engine/Core/Variant.h"

#include <string>
#include <utility>

namespace px::resource {

using ResourceId = Uuid;

// Stable resource identity used by Scenario/UI/Animation documents. The path
// is a repair hint only; identity remains valid when an asset is moved.
template <typename ResourceType = void>
class ResourceRef {
public:
    ResourceRef() = default;
    explicit ResourceRef(ResourceId id, std::string lastKnownPath = {})
        : m_id(id), m_lastKnownPath(std::move(lastKnownPath)) {}

    [[nodiscard]] const ResourceId& Id() const { return m_id; }
    [[nodiscard]] const std::string& LastKnownPath() const { return m_lastKnownPath; }
    [[nodiscard]] bool Empty() const { return m_id.Empty(); }
    explicit operator bool() const { return !Empty(); }

    [[nodiscard]] ResourceRefValue ToValue() const { return {m_id, m_lastKnownPath}; }
    [[nodiscard]] static ResourceRef FromValue(const ResourceRefValue& value) {
        return ResourceRef(value.id, value.lastKnownPath);
    }

    auto operator<=>(const ResourceRef&) const = default;

private:
    ResourceId m_id;
    std::string m_lastKnownPath;
};

}  // namespace px::resource
