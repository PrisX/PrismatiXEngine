#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace px::semver {

struct Version {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    auto operator<=>(const Version&) const = default;
};

[[nodiscard]] std::optional<Version> Parse(std::string_view text);

// Supports exact versions, comparison conjunctions/disjunctions, caret,
// tilde, and '*'. A null result means the range itself is malformed.
[[nodiscard]] std::optional<bool> Satisfies(
    const Version& version, std::string_view range);

}  // namespace px::semver
