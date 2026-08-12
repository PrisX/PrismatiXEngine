#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::sdk {

struct RuntimeIrOperation {
    std::string operationId;
    std::string sourceId;
    std::uint32_t sourceLine = 0;
    std::string kind;
    std::string text;
    std::unordered_map<std::string, std::string> arguments;
};

struct RuntimeIrDocument {
    std::string documentId;
    std::uint64_t committedRevision = 0;
    std::vector<RuntimeIrOperation> operations;
};

struct ContractDiagnostic {
    std::string code;
    std::string message;
    std::size_t operationIndex = 0;
};

struct RuntimeIrParseResult {
    RuntimeIrDocument document;
    std::vector<ContractDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

// Parses the public Studio-to-runtime contract. Contract revisions are strict:
// callers must never guess at newer fields or silently reinterpret old data.
[[nodiscard]] RuntimeIrParseResult ParseRuntimeIr(std::string_view json);

}  // namespace px::sdk
