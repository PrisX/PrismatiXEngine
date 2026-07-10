#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace px {

class Uuid {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    Uuid() = default;
    explicit Uuid(Bytes bytes) : m_bytes(bytes) {}

    [[nodiscard]] static Uuid Random();
    [[nodiscard]] static Uuid FromName(std::string_view name);
    [[nodiscard]] static std::optional<Uuid> Parse(std::string_view text);
    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] const Bytes& Data() const { return m_bytes; }

    auto operator<=>(const Uuid&) const = default;

private:
    Bytes m_bytes{};
};

struct UuidHash {
    [[nodiscard]] std::size_t operator()(const Uuid& value) const noexcept;
};

}  // namespace px
