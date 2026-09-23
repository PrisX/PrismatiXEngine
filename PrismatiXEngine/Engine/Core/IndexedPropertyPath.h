#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace px {

struct IndexedPropertyPathSegment {
    enum class Kind { Field, Index } kind = Kind::Field;
    std::string_view field;
    std::size_t index = 0;
};

// UI binding paths address typed ViewModel collections without executing an
// expression. Other authored property paths may deliberately stay dot-only.
inline std::optional<std::vector<IndexedPropertyPathSegment>> ParseIndexedPropertyPath(
    const std::string_view path) {
    if (path.empty() || path.size() > 512) return std::nullopt;
    const auto alpha = [](const char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') || value == '_';
    };
    const auto digit = [](const char value) {
        return value >= '0' && value <= '9';
    };
    std::vector<IndexedPropertyPathSegment> segments;
    std::size_t position = 0;
    const auto field = [&]() -> bool {
        if (position >= path.size() || !alpha(path[position])) return false;
        const std::size_t start = position++;
        while (position < path.size() &&
               (alpha(path[position]) || digit(path[position]))) ++position;
        segments.push_back({IndexedPropertyPathSegment::Kind::Field,
                            path.substr(start, position - start), 0});
        return true;
    };
    if (!field()) return std::nullopt;
    while (position < path.size()) {
        if (path[position] == '.') {
            ++position;
            if (!field()) return std::nullopt;
        } else if (path[position] == '[') {
            ++position;
            const std::size_t start = position;
            if (position >= path.size() || !digit(path[position])) return std::nullopt;
            if (path[position] == '0' && position + 1 < path.size() &&
                digit(path[position + 1])) return std::nullopt;
            while (position < path.size() && digit(path[position])) ++position;
            if (position >= path.size() || path[position] != ']') return std::nullopt;
            std::size_t index = 0;
            const auto number = path.substr(start, position - start);
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                return std::nullopt;
            segments.push_back({IndexedPropertyPathSegment::Kind::Index, {}, index});
            ++position;
        } else return std::nullopt;
    }
    return segments;
}

}  // namespace px
