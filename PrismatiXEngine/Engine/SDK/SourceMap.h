#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::sdk {

struct SourceMapMapping {
    std::string operationId;
    std::string sourceId;
    std::string sourceUri;
    std::uint32_t startLine = 0;
    std::uint32_t startColumn = 0;
    std::uint32_t endLine = 0;
    std::uint32_t endColumn = 0;
};

struct SourceMapDocument {
    std::string documentId;
    std::vector<SourceMapMapping> mappings;
    std::unordered_map<std::string, std::size_t> operationIndex;

    [[nodiscard]] const SourceMapMapping* Find(
        std::string_view operationId) const;
};

struct SourceMapDiagnostic {
    std::string code;
    std::string message;
    std::size_t mappingIndex = 0;
};

struct SourceMapParseResult {
    SourceMapDocument document;
    std::vector<SourceMapDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

// Parses the published RuntimeIR-to-authoring mapping. The parser is strict so
// Player diagnostics never fall back to a misleading or traversal-capable URI.
[[nodiscard]] SourceMapParseResult ParseSourceMap(std::string_view json);

}  // namespace px::sdk
