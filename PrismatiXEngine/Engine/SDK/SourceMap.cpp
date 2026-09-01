#include "Engine/SDK/SourceMap.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxSourceMapBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxMappings = 1'000'000U;
constexpr std::size_t kMaxIdentityBytes = 512U;
constexpr std::size_t kMaxSourceUriBytes = 4U * 1024U;

void Add(SourceMapParseResult& result, std::string code, std::string message,
         const std::size_t mappingIndex = 0) {
    result.diagnostics.push_back(
        {std::move(code), std::move(message), mappingIndex});
}

bool KnownRootField(const std::string_view key) {
    return key == "format" || key == "schemaRevision" ||
           key == "documentId" || key == "mappings";
}

bool KnownMappingField(const std::string_view key) {
    return key == "operationId" || key == "sourceId" ||
           key == "sourceUri" || key == "startLine" ||
           key == "startColumn" || key == "endLine" ||
           key == "endColumn";
}

bool ReadIdentity(const Json& object, const char* key, std::string& result) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->empty()) return false;
    result = found->get<std::string>();
    return result.size() <= kMaxIdentityBytes &&
           result.find('\0') == std::string::npos;
}

bool ReadPosition(const Json& object, const char* key, std::uint32_t& result) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) return false;
    const auto value = found->get<std::uint64_t>();
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) return false;
    result = static_cast<std::uint32_t>(value);
    return true;
}

bool SafeSourceUri(const std::string_view path) {
    if (path.empty() || path.size() > kMaxSourceUriBytes || path.front() == '/' ||
        path.find('\0') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < path.size()) {
        const auto separator = path.find('/', offset);
        const auto segment = path.substr(
            offset, separator == std::string_view::npos
                        ? path.size() - offset
                        : separator - offset);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (separator == std::string_view::npos) break;
        offset = separator + 1;
    }
    return true;
}

}  // namespace

const SourceMapMapping* SourceMapDocument::Find(
    const std::string_view operationId) const {
    const auto found = operationIndex.find(std::string(operationId));
    if (found == operationIndex.end() || found->second >= mappings.size()) return nullptr;
    return &mappings[found->second];
}

SourceMapParseResult ParseSourceMap(const std::string_view text) {
    SourceMapParseResult result;
    if (text.size() > kMaxSourceMapBytes) {
        Add(result, "PXSDKMAP1107", "Source map exceeds the size limit");
        return result;
    }
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        Add(result, "PXSDKMAP1101", "Source map must be a JSON object");
        return result;
    }
    for (auto field = root.begin(); field != root.end(); ++field) {
        if (!KnownRootField(field.key())) {
            Add(result, "PXSDKMAP1108", "Unknown source map field: " + field.key());
        }
    }
    if (root.value("format", std::string{}) != "PrismatiXSourceMap") {
        Add(result, "PXSDKMAP1102", "Source map format is not PrismatiXSourceMap");
    }
    const auto revision = root.find("schemaRevision");
    if (revision == root.end() || !revision->is_number_unsigned() ||
        revision->get<std::uint32_t>() != 2) {
        Add(result, "PXSDKMAP1103", "Unsupported source map schema revision");
    }
    if (!ReadIdentity(root, "documentId", result.document.documentId)) {
        Add(result, "PXSDKMAP1104", "Source map documentId is required");
    }
    const auto mappings = root.find("mappings");
    if (mappings == root.end() || !mappings->is_array()) {
        Add(result, "PXSDKMAP1105", "Source map mappings must be an array");
        return result;
    }
    if (mappings->size() > kMaxMappings) {
        Add(result, "PXSDKMAP1106", "Source map mapping count exceeds the limit");
        return result;
    }

    result.document.mappings.reserve(mappings->size());
    std::unordered_set<std::string> operationIds;
    for (std::size_t index = 0; index < mappings->size(); ++index) {
        const Json& item = (*mappings)[index];
        if (!item.is_object()) {
            Add(result, "PXSDKMAP1110", "Source map mapping must be an object", index);
            continue;
        }
        for (auto field = item.begin(); field != item.end(); ++field) {
            if (!KnownMappingField(field.key())) {
                Add(result, "PXSDKMAP1120",
                    "Unknown source map mapping field: " + field.key(), index);
            }
        }
        SourceMapMapping mapping;
        bool valid = true;
        if (!ReadIdentity(item, "operationId", mapping.operationId)) {
            Add(result, "PXSDKMAP1111", "Mapping operationId is required", index);
            valid = false;
        }
        if (!ReadIdentity(item, "sourceId", mapping.sourceId)) {
            Add(result, "PXSDKMAP1112", "Mapping sourceId is required", index);
            valid = false;
        }
        const auto sourceUri = item.find("sourceUri");
        if (sourceUri == item.end() || !sourceUri->is_string()) {
            Add(result, "PXSDKMAP1113", "Mapping sourceUri is required", index);
            valid = false;
        } else {
            mapping.sourceUri = sourceUri->get<std::string>();
            if (!SafeSourceUri(mapping.sourceUri)) {
                Add(result, "PXSDKMAP1114",
                    "Mapping sourceUri must be a safe project-relative path", index);
                valid = false;
            }
        }
        if (!ReadPosition(item, "startLine", mapping.startLine) ||
            !ReadPosition(item, "startColumn", mapping.startColumn) ||
            !ReadPosition(item, "endLine", mapping.endLine) ||
            !ReadPosition(item, "endColumn", mapping.endColumn)) {
            Add(result, "PXSDKMAP1115",
                "Mapping positions must be positive 32-bit integers", index);
            valid = false;
        } else if (mapping.endLine < mapping.startLine ||
                   (mapping.endLine == mapping.startLine &&
                    mapping.endColumn < mapping.startColumn)) {
            Add(result, "PXSDKMAP1116", "Mapping end precedes its start", index);
            valid = false;
        }
        if (!mapping.operationId.empty() &&
            !operationIds.insert(mapping.operationId).second) {
            Add(result, "PXSDKMAP1117", "Mapping operationId must be unique", index);
            valid = false;
        }
        if (valid) {
            result.document.operationIndex.emplace(
                mapping.operationId, result.document.mappings.size());
            result.document.mappings.push_back(std::move(mapping));
        }
    }
    return result;
}

}  // namespace px::sdk
