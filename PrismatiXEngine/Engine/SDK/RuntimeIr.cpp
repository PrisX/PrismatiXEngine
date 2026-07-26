#include "Engine/SDK/RuntimeIr.h"

#include <nlohmann/json.hpp>

#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::json;

void AddDiagnostic(RuntimeIrParseResult& result, std::string code,
                   std::string message, const std::size_t operationIndex = 0) {
    result.diagnostics.push_back(
        {std::move(code), std::move(message), operationIndex});
}

bool ReadRequiredString(const Json& object, const char* key, std::string& value) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->empty()) return false;
    value = found->get<std::string>();
    return true;
}

}  // namespace

RuntimeIrParseResult ParseRuntimeIr(const std::string_view text) {
    RuntimeIrParseResult result;
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result, "PXSDKIR1001", "Runtime IR must be a JSON object");
        return result;
    }
    if (root.value("format", std::string{}) != "PrismatiXRuntimeIR") {
        AddDiagnostic(result, "PXSDKIR1002", "Runtime IR format is not PrismatiXRuntimeIR");
    }
    const auto revision = root.find("schemaRevision");
    if (revision == root.end() || !revision->is_number_unsigned() ||
        revision->get<std::uint32_t>() != 1) {
        AddDiagnostic(result, "PXSDKIR1003", "Unsupported Runtime IR schema revision");
    }
    if (!ReadRequiredString(root, "documentId", result.document.documentId)) {
        AddDiagnostic(result, "PXSDKIR1004", "Runtime IR documentId is required");
    }
    const auto committedRevision = root.find("committedRevision");
    if (committedRevision == root.end() || !committedRevision->is_number_unsigned()) {
        AddDiagnostic(result, "PXSDKIR1005", "Runtime IR committedRevision is required");
    } else {
        result.document.committedRevision = committedRevision->get<std::uint64_t>();
    }

    const auto operations = root.find("operations");
    if (operations == root.end() || !operations->is_array()) {
        AddDiagnostic(result, "PXSDKIR1006", "Runtime IR operations must be an array");
        return result;
    }

    std::unordered_set<std::string> operationIds;
    std::unordered_set<std::string> sourceIds;
    result.document.operations.reserve(operations->size());
    for (std::size_t index = 0; index < operations->size(); ++index) {
        const Json& item = (*operations)[index];
        if (!item.is_object()) {
            AddDiagnostic(result, "PXSDKIR1010", "Runtime operation must be an object", index);
            continue;
        }
        RuntimeIrOperation operation;
        bool valid = true;
        if (!ReadRequiredString(item, "operationId", operation.operationId)) {
            AddDiagnostic(result, "PXSDKIR1011", "Runtime operationId is required", index);
            valid = false;
        }
        if (!ReadRequiredString(item, "sourceId", operation.sourceId)) {
            AddDiagnostic(result, "PXSDKIR1012", "Runtime sourceId is required", index);
            valid = false;
        }
        if (!ReadRequiredString(item, "kind", operation.kind)) {
            AddDiagnostic(result, "PXSDKIR1013", "Runtime operation kind is required", index);
            valid = false;
        }
        const auto operationText = item.find("text");
        if (operationText == item.end() || !operationText->is_string()) {
            AddDiagnostic(result, "PXSDKIR1014", "Runtime operation text must be a string", index);
            valid = false;
        } else {
            operation.text = operationText->get<std::string>();
        }
        const auto arguments = item.find("arguments");
        if (arguments == item.end() || !arguments->is_object()) {
            AddDiagnostic(result, "PXSDKIR1017", "Runtime operation arguments must be an object", index);
            valid = false;
        } else {
            for (auto argument = arguments->begin(); argument != arguments->end(); ++argument) {
                if (!argument.value().is_string()) {
                    AddDiagnostic(result, "PXSDKIR1018",
                                  "Runtime operation arguments must contain string values", index);
                    valid = false;
                    continue;
                }
                operation.arguments.emplace(argument.key(), argument.value().get<std::string>());
            }
        }
        if (!operation.operationId.empty() && !operationIds.insert(operation.operationId).second) {
            AddDiagnostic(result, "PXSDKIR1015", "Runtime operationId must be unique", index);
            valid = false;
        }
        if (!operation.sourceId.empty() && !sourceIds.insert(operation.sourceId).second) {
            AddDiagnostic(result, "PXSDKIR1016", "Runtime sourceId must be unique", index);
            valid = false;
        }
        if (valid) result.document.operations.push_back(std::move(operation));
    }
    return result;
}

}  // namespace px::sdk
