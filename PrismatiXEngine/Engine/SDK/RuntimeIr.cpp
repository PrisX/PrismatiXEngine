#include "Engine/SDK/RuntimeIr.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <string_view>
#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::json;

// Compiled projects aggregate chapters: 100k authored lines can exceed 32 MiB.
// Match the bounded source-map document budget; operation/string limits remain.
constexpr std::size_t kMaxRuntimeIrBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxRuntimeOperations = 1'000'000U;
constexpr std::size_t kMaxOperationArguments = 1'024U;
constexpr std::size_t kMaxRuntimeStringBytes = 1U * 1024U * 1024U;

bool ExecutableOperationKind(const std::string_view kind) {
    static constexpr std::string_view kinds[] = {
        "scene",         "fragment",      "callFragment",
        "return",        "endStory",      "dialogue",
        "narration",     "choiceOption",  "background",
        "showCharacter", "hideCharacter", "hideCharacters", "voice",
        "bgm",           "stopBgm",       "soundEffect",
        "ambience",      "stopAmbience",  "video",
        "setVariable",
        "condition",     "else",           "endCondition",
        "label",         "jump",           "wait",
        "timeline",      "effect",         "shake",          "ui",
        "hudVisibility", "image",        "hideImage",      "transition", "actorAction",   "stopSoundEffect", "stopVoice",
        "unlock",        "customNode",     "camera",         "textMode",       "autoMode",       "skipControl",
    };
    for (const auto candidate : kinds)
        if (candidate == kind) return true;
    return false;
}

bool KnownRootField(const std::string_view key) {
    return key == "format" || key == "schemaRevision" ||
           key == "documentId" || key == "committedRevision" ||
           key == "operations";
}

bool KnownOperationField(const std::string_view key) {
    return key == "operationId" || key == "sourceId" ||
           key == "sourceLine" || key == "kind" || key == "text" ||
           key == "arguments";
}

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

// As ReadRequiredString, moving the string out of the parsed document.
bool TakeRequiredString(Json& object, const char* key, std::string& value) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->empty()) return false;
    value = std::move(found->get_ref<std::string&>());
    return true;
}

}  // namespace

RuntimeIrParseResult ParseRuntimeIr(const std::string_view text) {
    RuntimeIrParseResult result;
    if (text.size() > kMaxRuntimeIrBytes) {
        AddDiagnostic(result, "PXSDKIR1007", "Runtime IR exceeds the size limit");
        return result;
    }
    // Not const: each operation's strings are moved out rather than copied.
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result, "PXSDKIR1001", "Runtime IR must be a JSON object");
        return result;
    }
    for (auto field = root.begin(); field != root.end(); ++field) {
        if (!KnownRootField(field.key()))
            AddDiagnostic(result, "PXSDKIR1008",
                          "Unknown Runtime IR field: " + field.key());
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

    auto operations = root.find("operations");
    if (operations == root.end() || !operations->is_array()) {
        AddDiagnostic(result, "PXSDKIR1006", "Runtime IR operations must be an array");
        return result;
    }
    if (operations->size() > kMaxRuntimeOperations) {
        AddDiagnostic(result, "PXSDKIR1009",
                      "Runtime IR operation count exceeds the limit");
        return result;
    }

    // Views of the stored operations' ids: the vector is reserved, so they
    // stay where they are.
    std::unordered_set<std::string_view> operationIds;
    std::unordered_set<std::string_view> sourceIds;
    operationIds.reserve(operations->size());
    sourceIds.reserve(operations->size());
    result.document.operations.reserve(operations->size());
    for (std::size_t index = 0; index < operations->size(); ++index) {
        Json& item = (*operations)[index];
        if (!item.is_object()) {
            AddDiagnostic(result, "PXSDKIR1010", "Runtime operation must be an object", index);
            continue;
        }
        for (auto field = item.begin(); field != item.end(); ++field) {
            if (!KnownOperationField(field.key())) {
                AddDiagnostic(result, "PXSDKIR1020",
                              "Unknown Runtime operation field: " + field.key(),
                              index);
            }
        }
        RuntimeIrOperation operation;
        bool valid = true;
        if (!TakeRequiredString(item, "operationId", operation.operationId)) {
            AddDiagnostic(result, "PXSDKIR1011", "Runtime operationId is required", index);
            valid = false;
        }
        if (!TakeRequiredString(item, "sourceId", operation.sourceId)) {
            AddDiagnostic(result, "PXSDKIR1012", "Runtime sourceId is required", index);
            valid = false;
        }
        const auto sourceLine = item.find("sourceLine");
        if (sourceLine == item.end()) {
            operation.sourceLine = static_cast<std::uint32_t>(index + 1);
        } else if (!sourceLine->is_number_unsigned() ||
                   sourceLine->get<std::uint64_t>() == 0 ||
                   sourceLine->get<std::uint64_t>() >
                       std::numeric_limits<std::uint32_t>::max()) {
            AddDiagnostic(result, "PXSDKIR1019",
                          "Runtime sourceLine must be a positive 32-bit integer", index);
            valid = false;
        } else {
            operation.sourceLine = sourceLine->get<std::uint32_t>();
        }
        if (!TakeRequiredString(item, "kind", operation.kind)) {
            AddDiagnostic(result, "PXSDKIR1013", "Runtime operation kind is required", index);
            valid = false;
        } else if (!ExecutableOperationKind(operation.kind)) {
            AddDiagnostic(result, "PXSDKIR1021",
                          "Unknown or structural Runtime operation kind: " +
                              operation.kind,
                          index);
            valid = false;
        }
        const auto operationText = item.find("text");
        if (operationText == item.end() || !operationText->is_string()) {
            AddDiagnostic(result, "PXSDKIR1014", "Runtime operation text must be a string", index);
            valid = false;
        } else {
            operation.text = std::move(operationText->get_ref<std::string&>());
            if (operation.text.size() > kMaxRuntimeStringBytes) {
                AddDiagnostic(result, "PXSDKIR1022",
                              "Runtime operation text exceeds the size limit", index);
                valid = false;
            }
        }
        auto arguments = item.find("arguments");
        if (arguments == item.end() || !arguments->is_object()) {
            AddDiagnostic(result, "PXSDKIR1017", "Runtime operation arguments must be an object", index);
            valid = false;
        } else {
            if (arguments->size() > kMaxOperationArguments) {
                AddDiagnostic(result, "PXSDKIR1023",
                              "Runtime operation argument count exceeds the limit",
                              index);
                valid = false;
            }
            operation.arguments.reserve(arguments->size());
            for (auto argument = arguments->begin(); argument != arguments->end(); ++argument) {
                if (!argument.value().is_string()) {
                    AddDiagnostic(result, "PXSDKIR1018",
                                  "Runtime operation arguments must contain string values", index);
                    valid = false;
                    continue;
                }
                std::string& value = argument.value().get_ref<std::string&>();
                if (argument.key().size() > kMaxRuntimeStringBytes ||
                    value.size() > kMaxRuntimeStringBytes) {
                    AddDiagnostic(result, "PXSDKIR1024",
                                  "Runtime operation argument exceeds the size limit",
                                  index);
                    valid = false;
                    continue;
                }
                operation.arguments.emplace(argument.key(), std::move(value));
            }
        }
        if (!valid) continue;
        const RuntimeIrOperation& stored = result.document.operations.emplace_back(std::move(operation));
        const bool newOperation = !operationIds.contains(stored.operationId);
        const bool newSource = !sourceIds.contains(stored.sourceId);
        if (!newOperation)
            AddDiagnostic(result, "PXSDKIR1015", "Runtime operationId must be unique", index);
        if (!newSource)
            AddDiagnostic(result, "PXSDKIR1016", "Runtime sourceId must be unique", index);
        if (newOperation && newSource) {
            operationIds.insert(stored.operationId);
            sourceIds.insert(stored.sourceId);
        } else {
            result.document.operations.pop_back();
        }
    }
    return result;
}

}  // namespace px::sdk
