#include "Engine/Core/SemanticVersion.h"

#include <charconv>
#include <cctype>

namespace px::semver {
namespace {

std::string_view Trim(const std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

std::optional<bool> Compare(const Version& actual, std::string_view token) {
    token = Trim(token);
    if (token.empty() || token == "*") return true;
    std::string_view operation = "=";
    for (const auto candidate : {std::string_view(">="), std::string_view("<="),
                                 std::string_view(">"), std::string_view("<"),
                                 std::string_view("="), std::string_view("^"),
                                 std::string_view("~")}) {
        if (token.starts_with(candidate)) {
            operation = candidate;
            token.remove_prefix(candidate.size());
            break;
        }
    }
    const auto required = Parse(token);
    if (!required) return std::nullopt;
    if (operation == ">=") return actual >= *required;
    if (operation == "<=") return actual <= *required;
    if (operation == ">") return actual > *required;
    if (operation == "<") return actual < *required;
    if (operation == "=") return actual == *required;
    if (operation == "~") {
        const Version upper{required->major, required->minor + 1, 0};
        return actual >= *required && actual < upper;
    }
    Version upper;
    if (required->major > 0) upper = {required->major + 1, 0, 0};
    else if (required->minor > 0) upper = {0, required->minor + 1, 0};
    else upper = {0, 0, required->patch + 1};
    return actual >= *required && actual < upper;
}

}  // namespace

std::optional<Version> Parse(std::string_view text) {
    text = Trim(text);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
        text.remove_prefix(1);
    const auto suffix = text.find_first_of("-+");
    const auto core = text.substr(0, suffix);
    Version version;
    std::uint64_t* fields[] = {&version.major, &version.minor, &version.patch};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        const std::size_t end = core.find('.', offset);
        const auto part = core.substr(offset,
            end == std::string_view::npos ? core.size() - offset : end - offset);
        if (part.empty()) return std::nullopt;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(),
                                            *fields[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
            return std::nullopt;
        if (index < 2) {
            if (end == std::string_view::npos) return std::nullopt;
            offset = end + 1;
        } else if (end != std::string_view::npos) {
            return std::nullopt;
        }
    }
    if (suffix != std::string_view::npos && suffix + 1 == text.size())
        return std::nullopt;
    return version;
}

std::optional<bool> Satisfies(const Version& version,
                              const std::string_view range) {
    std::size_t clauseStart = 0;
    while (clauseStart <= range.size()) {
        const std::size_t clauseEnd = range.find("||", clauseStart);
        const auto clause = Trim(range.substr(
            clauseStart, clauseEnd == std::string_view::npos
                             ? range.size() - clauseStart
                             : clauseEnd - clauseStart));
        if (clause.empty()) return std::nullopt;
        bool matches = true;
        std::size_t tokenStart = 0;
        while (tokenStart < clause.size()) {
            while (tokenStart < clause.size() &&
                   std::isspace(static_cast<unsigned char>(clause[tokenStart])))
                ++tokenStart;
            std::size_t tokenEnd = tokenStart;
            while (tokenEnd < clause.size() &&
                   !std::isspace(static_cast<unsigned char>(clause[tokenEnd])))
                ++tokenEnd;
            if (tokenStart == tokenEnd) break;
            const auto comparison =
                Compare(version, clause.substr(tokenStart, tokenEnd - tokenStart));
            if (!comparison) return std::nullopt;
            matches = matches && *comparison;
            tokenStart = tokenEnd;
        }
        if (matches) return true;
        if (clauseEnd == std::string_view::npos) break;
        clauseStart = clauseEnd + 2;
    }
    return false;
}

}  // namespace px::semver
