#include "Engine/SDK/GameCatalogResources.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace px::sdk {
namespace {

constexpr std::size_t kMaxCatalogBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxEntries = 1'000'000;
constexpr std::size_t kMaxTextBytes = 4096;
constexpr std::size_t kMaxProjectManifestBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxAssets = 1'000'000;

using Json = nlohmann::json;

struct ProjectAsset {
    std::string id;
    std::string source;
    std::string kind;
};

struct RawProperty {
    std::string value;
    int line = 0;
};

struct RawNode {
    std::string id;
    std::string parent;
    std::string name;
    std::string type;
    int line = 0;
    std::unordered_map<std::string, RawProperty> properties;
};

std::string Trim(const std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return std::string(value.substr(first, last - first + 1));
}

void Add(GameCatalogResourcesLoadResult& result, std::string code, std::string message, const std::string& path, const int line = 0, std::string nodeId = {}, std::string property = {}) {
    result.diagnostics.push_back({ .code = std::move(code), .message = std::move(message), .path = path, .nodeId = std::move(nodeId), .property = std::move(property), .line = line });
}

bool IsUuid(const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) continue;
        if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) return false;
    }
    return true;
}

bool IsSafeUri(const std::string_view path) {
    if (path.empty() || path.size() > kMaxTextBytes || path.front() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(
            start, end == std::string_view::npos ? path.size() - start
                                                  : end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::string ParseQuoted(const std::string_view value, bool& valid) {
    valid = false;
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return {};
    std::string output;
    output.reserve(value.size() - 2);
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        if (value[index] != '\\') {
            output.push_back(value[index]);
            continue;
        }
        ++index;
        if (index + 1 > value.size()) return {};
        switch (value[index]) {
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '"':
                output.push_back('"');
                break;
            default:
                return {};
        }
    }
    valid = true;
    return output;
}

std::vector<std::string> SplitArguments(const std::string_view body) {
    std::vector<std::string> result;
    std::size_t start = 0;
    bool quoted = false;
    int depth = 0;
    for (std::size_t index = 0; index <= body.size(); ++index) {
        if (index < body.size() && body[index] == '"' && (index == 0 || body[index - 1] != '\\')) {
            quoted = !quoted;
        }
        if (!quoted && index < body.size()) {
            if (body[index] == '(' || body[index] == '[' || body[index] == '{') {
                ++depth;
            }
            else if (body[index] == ')' || body[index] == ']' || body[index] == '}') {
                --depth;
            }
        }
        if (index == body.size() || (!quoted && depth == 0 && body[index] == ',')) {
            result.push_back(Trim(body.substr(start, index - start)));
            start = index + 1;
        }
    }
    return result;
}

const RawProperty* Property(const RawNode& node, const char* key) {
    const auto found = node.properties.find(key);
    return found == node.properties.end() ? nullptr : &found->second;
}

bool ReadString(GameCatalogResourcesLoadResult& result, const RawNode& node, const char* key, std::string& output, const std::string& path, const bool required) {
    const auto* property = Property(node, key);
    if (!property) {
        if (required) {
            Add(result, "PXCAT1010", node.type + " requires a non-empty '" + key + "' string", path, node.line, node.id, key);
        }
        return !required;
    }
    bool valid = false;
    auto value = ParseQuoted(property->value, valid);
    if (!valid || value.size() > kMaxTextBytes || (required && Trim(value).empty())) {
        Add(result, required ? "PXCAT1010" : "PXCAT1011", std::string(node.type) + " '" + key + (required ? "' must be a non-empty bounded string" : "' must be a bounded string when present"), path, property->line, node.id, key);
        return false;
    }
    output = std::move(value);
    return true;
}

bool ReadInteger(GameCatalogResourcesLoadResult& result, const RawNode& node, const char* key, int& output, const std::string& path) {
    const auto* property = Property(node, key);
    if (!property) return true;
    std::int64_t value = 0;
    const auto parsed = std::from_chars(property->value.data(), property->value.data() + property->value.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != property->value.data() + property->value.size() || value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)()) {
        Add(result, "PXCAT1012", std::string(node.type) + " '" + key + "' must be a 32-bit integer when present", path, property->line, node.id, key);
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

bool ReadBoolean(GameCatalogResourcesLoadResult& result, const RawNode& node, const char* key, bool& output, const std::string& path) {
    const auto* property = Property(node, key);
    if (!property) return true;
    if (property->value == "true") {
        output = true;
        return true;
    }
    if (property->value == "false") {
        output = false;
        return true;
    }
    Add(result, "PXCAT1013", std::string(node.type) + " '" + key + "' must be a boolean when present", path, property->line, node.id, key);
    return false;
}

bool ReadAssetReference(
    GameCatalogResourcesLoadResult& result, const RawNode& node,
    const char* key, GameCatalogAssetReference& output,
    const std::string& path, const bool required,
    const LegacyGalleryReferencePolicy policy) {
    const auto* property = Property(node, key);
    if (!property) {
        if (required) {
            Add(result, "PXCAT1010",
                node.type + " requires a non-empty '" + key +
                    "' ResourceRef",
                path, node.line, node.id, key);
        }
        return !required;
    }

    const std::string value = Trim(property->value);
    if (value.starts_with("res(") && value.ends_with(')')) {
        const auto arguments = SplitArguments(
            std::string_view(value).substr(4, value.size() - 5));
        bool idValid = false;
        bool pathValid = false;
        const std::string id =
            arguments.size() == 2
                ? ParseQuoted(arguments[0], idValid)
                : std::string{};
        const std::string hint =
            arguments.size() == 2
                ? ParseQuoted(arguments[1], pathValid)
                : std::string{};
        if (arguments.size() != 2 || !idValid || !pathValid || !IsUuid(id) ||
            (!hint.empty() && !IsSafeUri(hint))) {
            Add(result, "PXCAT1018",
                std::string(node.type) + " '" + key +
                    "' must be res(\"<uuid>\", \"<project-relative hint>\")",
                path, property->line, node.id, key);
            return false;
        }
        output = {.assetId = id, .assetPath = hint};
        return true;
    }

    bool stringValid = false;
    std::string legacyPath = ParseQuoted(value, stringValid);
    if (!stringValid || !IsSafeUri(legacyPath)) {
        Add(result, "PXCAT1018",
            std::string(node.type) + " '" + key +
                "' must be a ResourceRef",
            path, property->line, node.id, key);
        return false;
    }
    if (policy == LegacyGalleryReferencePolicy::RejectPathStrings) {
        Add(result, "PXCAT1019",
            std::string(node.type) + " '" + key +
                "' still uses a path string; run the one-time Gallery ResourceRef migration",
            path, property->line, node.id, key);
        return false;
    }
    output = {.assetPath = std::move(legacyPath)};
    return true;
}

bool ImageKind(const std::string_view kind) {
    return kind == "background" || kind == "character" || kind == "cg" ||
           kind == "uiImage";
}

bool JsonRevisionOne(const Json& manifest) {
    const auto revision = manifest.find("schemaRevision");
    if (revision == manifest.end()) return false;
    if (revision->is_number_unsigned()) {
        return revision->get<std::uint64_t>() == 1;
    }
    return revision->is_number_integer() &&
           revision->get<std::int64_t>() == 1;
}

std::optional<std::unordered_map<std::string, ProjectAsset>> ParseProjectAssets(
    GameCatalogResourcesLoadResult& result,
    const std::string_view projectManifest) {
    if (projectManifest.empty() ||
        projectManifest.size() > kMaxProjectManifestBytes) {
        Add(result, "PXCAT1020",
            "Gallery ResourceRefs require a bounded project.pxproject manifest",
            "project.pxproject");
        return std::nullopt;
    }
    const Json manifest = Json::parse(projectManifest, nullptr, false);
    const auto format = manifest.is_object() ? manifest.find("format")
                                             : manifest.end();
    if (manifest.is_discarded() || !manifest.is_object() ||
        format == manifest.end() || !format->is_string() ||
        format->get_ref<const std::string&>() != "PrismatiXProject" ||
        !JsonRevisionOne(manifest)) {
        Add(result, "PXCAT1020",
            "project.pxproject must be PrismatiXProject schema revision 1",
            "project.pxproject");
        return std::nullopt;
    }
    const auto descriptors = manifest.find("assets");
    if (descriptors == manifest.end() || !descriptors->is_array() ||
        descriptors->size() > kMaxAssets) {
        Add(result, "PXCAT1021",
            "project.pxproject assets must be an array within the contract limit",
            "project.pxproject");
        return std::nullopt;
    }

    std::unordered_map<std::string, ProjectAsset> assets;
    std::unordered_set<std::string> sources;
    for (const auto& descriptor : *descriptors) {
        const auto id = descriptor.is_object() ? descriptor.find("id")
                                               : descriptor.end();
        const auto source = descriptor.is_object()
                                ? descriptor.find("source")
                                : descriptor.end();
        const auto kind = descriptor.is_object() ? descriptor.find("kind")
                                                 : descriptor.end();
        if (!descriptor.is_object() || id == descriptor.end() ||
            !id->is_string() || !IsUuid(id->get_ref<const std::string&>()) ||
            source == descriptor.end() || !source->is_string() ||
            !IsSafeUri(source->get_ref<const std::string&>()) ||
            kind == descriptor.end() || !kind->is_string() || kind->empty()) {
            Add(result, "PXCAT1022",
                "project asset descriptors require UUID id, safe source, and kind",
                "project.pxproject");
            continue;
        }
        ProjectAsset asset{id->get<std::string>(), source->get<std::string>(),
                           kind->get<std::string>()};
        if (!sources.insert(asset.source).second) {
            Add(result, "PXCAT1023",
                "duplicate project asset source: " + asset.source,
                "project.pxproject", 0, {}, "assets");
        }
        if (!assets.emplace(asset.id, std::move(asset)).second) {
            Add(result, "PXCAT1023",
                "duplicate project asset UUID: " + id->get<std::string>(),
                "project.pxproject", 0, {}, "assets");
        }
    }
    if (!result.diagnostics.empty()) return std::nullopt;
    return assets;
}

bool ResolveGalleryReference(
    GameCatalogResourcesLoadResult& result,
    GameCatalogAssetReference& reference,
    const std::unordered_map<std::string, ProjectAsset>& assets,
    const GameCatalogResourceExists& exists, const std::string& path,
    const GameCatalogGalleryItem& item, const char* property) {
    const auto found = assets.find(reference.assetId);
    if (found == assets.end()) {
        Add(result, "PXCAT1024",
            "GalleryItem references an unknown asset UUID: " +
                reference.assetId,
            path, item.sourceLine, item.nodeId, property);
        return false;
    }
    if (!ImageKind(found->second.kind)) {
        Add(result, "PXCAT1025",
            "GalleryItem asset " + reference.assetId +
                " has incompatible kind '" + found->second.kind + "'",
            path, item.sourceLine, item.nodeId, property);
        return false;
    }
    if (!exists || !exists(found->second.source)) {
        Add(result, "PXCAT1026",
            "GalleryItem asset file is missing: " + found->second.source,
            path, item.sourceLine, item.nodeId, property);
        return false;
    }
    reference.assetPath = found->second.source;
    return true;
}

}  // namespace

GameCatalogResourcesLoadResult LoadGameCatalogResources(
    const std::string_view typedResource, std::string path,
    const LegacyGameCatalogPolicy legacyPolicy,
    const LegacyGalleryReferencePolicy galleryPolicy) {
    GameCatalogResourcesLoadResult result;
    if (typedResource.size() > kMaxCatalogBytes) {
        Add(result, "PXCAT1000", "Game.pxres exceeds the 16 MiB runtime catalog contract limit", path);
        return result;
    }

    std::istringstream input{ std::string(typedResource) };
    std::string raw;
    bool headerSeen = false;
    RawNode* activeNode = nullptr;
    std::vector<RawNode> nodes;
    std::unordered_set<std::string> nodeIds;
    int line = 0;
    while (std::getline(input, raw)) {
        ++line;
        const std::string current = Trim(raw);
        if (current.empty() || current.starts_with('#')) continue;
        if (!headerSeen) {
            std::istringstream header(current);
            std::string kind;
            std::string version;
            std::string id;
            std::string type;
            std::string extra;
            header >> kind >> version >> id >> type;
            if (kind != "@pxresource" || version != "4" || !IsUuid(id) || type != "GameCatalog" || (header >> extra)) {
                Add(result, "PXCAT1001", "expected strict '@pxresource 4 <UUID> GameCatalog' header", path, line);
            }
            else {
                result.document.documentId = std::move(id);
            }
            headerSeen = true;
            continue;
        }
        if (current.starts_with("[node ") && current.ends_with(']')) {
            if (nodes.size() >= kMaxEntries) {
                Add(result, "PXCAT1002", "Game.pxres exceeds the runtime entry limit", path, line);
                activeNode = nullptr;
                continue;
            }
            const auto parts = SplitArguments(std::string_view(current).substr(6, current.size() - 7));
            if (parts.size() != 4) {
                Add(result, "PXCAT1003", "a node header requires id, parent, name, and type", path, line);
                activeNode = nullptr;
                continue;
            }
            bool idValid = false;
            bool parentValid = false;
            bool nameValid = false;
            bool typeValid = false;
            auto id = ParseQuoted(parts[0], idValid);
            auto parent = ParseQuoted(parts[1], parentValid);
            auto name = ParseQuoted(parts[2], nameValid);
            auto type = ParseQuoted(parts[3], typeValid);
            if (!idValid || !parentValid || !nameValid || !typeValid || !IsUuid(id) || (!parent.empty() && !IsUuid(parent))) {
                Add(result, "PXCAT1004", "node identity is invalid", path, line, id);
                activeNode = nullptr;
                continue;
            }
            if (!nodeIds.insert(id).second) {
                Add(result, "PXCAT1005", "duplicate node UUID: " + id, path, line, id);
            }
            nodes.push_back({ .id = std::move(id), .parent = std::move(parent), .name = std::move(name), .type = std::move(type), .line = line });
            activeNode = &nodes.back();
            continue;
        }
        const auto equals = current.find('=');
        if (equals == std::string::npos) {
            Add(result, "PXCAT1006", "expected a property assignment", path, line, activeNode ? activeNode->id : std::string{});
            continue;
        }
        const std::string key = Trim(std::string_view(current).substr(0, equals));
        const std::string value = Trim(std::string_view(current).substr(equals + 1));
        if (!activeNode) {
            Add(result, "PXCAT1007", "document-level GameCatalog properties are not supported", path, line, {}, key);
            continue;
        }
        if (key.empty() || value.empty()) {
            Add(result, "PXCAT1006", "property key and value are required", path, line, activeNode->id, key);
            continue;
        }
        if (!activeNode->properties.emplace(key, RawProperty{ value, line }).second) {
            Add(result, "PXCAT1008", "duplicate property: " + key, path, line, activeNode->id, key);
        }
    }
    if (!headerSeen) {
        Add(result, "PXCAT1001", "Game.pxres is empty or missing its typed header", path, 1);
        return result;
    }

    std::unordered_set<std::string> variableNames;
    std::unordered_set<std::string> galleryIds;
    for (const auto& node : nodes) {
        if (node.type == "Character" || node.type == "CharacterExpression") {
            result.legacyCharacterNodesPresent = true;
            if (legacyPolicy == LegacyGameCatalogPolicy::RejectCharacterNodes) {
                Add(result, "PXCAT1009", "characterResources projects cannot retain " + node.type + " nodes in Content/Game.pxres", path, node.line, node.id);
            }
            continue;
        }
        if (node.type != "InputBinding" && node.type != "Variable" && node.type != "GalleryItem") {
            Add(result, "PXCAT1014", "unsupported GameCatalog entry type: " + node.type, path, node.line, node.id);
            continue;
        }
        if (!node.parent.empty()) {
            Add(result, "PXCAT1015", node.type + " must be a root GameCatalog entry", path, node.line, node.id);
            continue;
        }
        if (node.type == "InputBinding") {
            GameCatalogInputBinding value;
            value.nodeId = node.id;
            const bool key = ReadString(result, node, "key", value.key, path, true);
            const bool command = ReadString(result, node, "command", value.command, path, true);
            const bool argument = ReadString(result, node, "argument", value.argument, path, false);
            if (key && command && argument) result.document.inputBindings.push_back(std::move(value));
            continue;
        }
        if (node.type == "Variable") {
            GameCatalogVariable value;
            value.nodeId = node.id;
            const bool name = ReadString(result, node, "name", value.name, path, true);
            const bool defaultValue = ReadInteger(result, node, "default", value.defaultValue, path);
            const bool persistent = ReadBoolean(result, node, "persistent", value.persistent, path);
            if (name && !variableNames.insert(value.name).second) {
                Add(result, "PXCAT1016", "duplicate Variable name: " + value.name, path, node.line, node.id, "name");
            }
            else if (name && defaultValue && persistent) {
                result.document.variables.push_back(std::move(value));
            }
            continue;
        }
        GameCatalogGalleryItem value;
        value.nodeId = node.id;
        value.sourceLine = node.line;
        const bool id = ReadString(result, node, "id", value.id, path, true);
        const bool title = ReadString(result, node, "title", value.title, path, false);
        const bool image = ReadAssetReference(
            result, node, "image", value.image, path, true, galleryPolicy);
        GameCatalogAssetReference thumbnailValue;
        const bool thumbnail = ReadAssetReference(
            result, node, "thumbnail", thumbnailValue, path, false,
            galleryPolicy);
        if (Property(node, "thumbnail") && thumbnail) {
            value.thumbnail = std::move(thumbnailValue);
        }
        if (id && !galleryIds.insert(value.id).second) {
            Add(result, "PXCAT1017", "duplicate GalleryItem id: " + value.id, path, node.line, node.id, "id");
        }
        else if (id && title && image && thumbnail) {
            result.document.gallery.push_back(std::move(value));
        }
    }
    return result;
}

GameCatalogResourcesLoadResult ResolveGameCatalogGalleryResources(
    GameCatalogResourcesDocument document,
    const std::string_view projectManifest,
    const GameCatalogResourceExists& exists, std::string path) {
    GameCatalogResourcesLoadResult result;
    result.document = std::move(document);
    auto assets = ParseProjectAssets(result, projectManifest);
    if (!assets) return result;
    for (auto& item : result.document.gallery) {
        (void)ResolveGalleryReference(result, item.image, *assets, exists, path,
                                      item, "image");
        if (item.thumbnail) {
            (void)ResolveGalleryReference(result, *item.thumbnail, *assets,
                                          exists, path, item, "thumbnail");
        }
    }
    return result;
}

}  // namespace px::sdk
