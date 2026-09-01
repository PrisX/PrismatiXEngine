#include "Engine/Session/RuntimeAssetResolver.h"

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/Core/Uuid.h"

namespace px {
namespace {

using Json = nlohmann::json;
constexpr std::string_view kAssetPrefix = "asset:";
constexpr std::size_t kMaxProjectManifestBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxAssets = 1'000'000;

struct ProjectAsset {
    Uuid id;
    std::string path;
};

using ProjectAssets = std::unordered_map<Uuid, ProjectAsset, UuidHash>;

diag::Diagnostic Error(std::string code, std::string message, const std::string& path, const int line = 0, std::string property = {}, std::string resourceId = {}, std::string details = {}) {
    diag::Diagnostic diagnostic{ .severity = diag::Severity::Error, .code = std::move(code), .category = "Runtime.ResourceRef", .message = std::move(message), .details = std::move(details) };
    diagnostic.source.path = path;
    diagnostic.source.line = line;
    diagnostic.source.property = std::move(property);
    diagnostic.source.resourceId = std::move(resourceId);
    return diagnostic;
}

bool IsSafeProjectUri(const std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

bool IsRevisionTwo(const Json& manifest) {
    const auto revision = manifest.find("schemaRevision");
    if (revision == manifest.end()) return false;
    if (revision->is_number_unsigned()) return revision->get<std::uint64_t>() == 2;
    return revision->is_number_integer() && revision->get<std::int64_t>() == 2;
}

Result<ProjectAssets> ParseProjectAssets(const std::string_view manifestText) {
    if (manifestText.empty()) {
        return Result<ProjectAssets>::Failure(Error("PXRUNTIME7310", "Runtime IR asset references require project.pxproject", "project.pxproject"));
    }
    if (manifestText.size() > kMaxProjectManifestBytes) {
        return Result<ProjectAssets>::Failure(Error("PXRUNTIME7311", "project.pxproject exceeds the 16 MiB runtime asset contract limit", "project.pxproject"));
    }
    const Json manifest = Json::parse(manifestText, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        return Result<ProjectAssets>::Failure(Error("PXRUNTIME7311", "project.pxproject is not valid JSON", "project.pxproject"));
    }
    const auto format = manifest.find("format");
    if (format == manifest.end() || !format->is_string() || format->get_ref<const std::string&>() != "PrismatiXProject" || !IsRevisionTwo(manifest)) {
        return Result<ProjectAssets>::Failure(Error("PXRUNTIME7312", "project.pxproject must be PrismatiXProject schema revision 2", "project.pxproject"));
    }
    const auto descriptors = manifest.find("assets");
    if (descriptors == manifest.end() || !descriptors->is_array() || descriptors->size() > kMaxAssets) {
        return Result<ProjectAssets>::Failure(Error("PXRUNTIME7313", "project.pxproject assets must be an array within the runtime contract limit", "project.pxproject"));
    }

    ProjectAssets assets;
    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& descriptor : *descriptors) {
        if (!descriptor.is_object()) {
            diagnostics.push_back(Error("PXRUNTIME7314", "Project asset descriptors require a UUID id and safe source", "project.pxproject"));
            continue;
        }
        const auto idValue = descriptor.find("id");
        const auto sourceValue = descriptor.find("source");
        const std::string idText = idValue != descriptor.end() && idValue->is_string() ? idValue->get<std::string>() : std::string{};
        const std::string source = sourceValue != descriptor.end() && sourceValue->is_string() ? sourceValue->get<std::string>() : std::string{};
        const auto id = Uuid::Parse(idText);
        if (!id || !IsSafeProjectUri(source)) {
            diagnostics.push_back(Error("PXRUNTIME7314", "Project asset descriptors require a UUID id and safe source", "project.pxproject", 0, "assets", idText, source.empty() ? idText : source));
            continue;
        }
        if (!assets.emplace(*id, ProjectAsset{ *id, source }).second) {
            diagnostics.push_back(Error("PXRUNTIME7315", "Duplicate project asset UUID: " + idText, "project.pxproject", 0, "assets", idText));
        }
    }
    if (!diagnostics.empty()) {
        return Result<ProjectAssets>::Failure(std::move(diagnostics));
    }
    return Result<ProjectAssets>::Success(std::move(assets));
}

bool IsAssetToken(const std::string_view value) { return value.starts_with(kAssetPrefix); }

bool UsesAssetReference(const Variant& value) {
    if (const auto* text = value.TryGet<std::string>()) return IsAssetToken(*text);
    if (const auto* reference = value.TryGet<ResourceRefValue>()) {
        return !reference->id.Empty() || IsAssetToken(reference->lastKnownPath);
    }
    if (const auto* array = value.AsArray()) {
        return std::any_of(array->begin(), array->end(), UsesAssetReference);
    }
    if (const auto* object = value.AsObject()) {
        return std::any_of(object->begin(), object->end(), [](const auto& field) { return UsesAssetReference(field.second); });
    }
    return false;
}

std::optional<ResourceRefValue> ResolveId(const Uuid& id, const ProjectAssets& assets, const RuntimeAssetExists& exists, const std::string& sourcePath, const int line, const std::string& property, std::vector<diag::Diagnostic>& diagnostics) {
    const std::string idText = id.ToString();
    const auto found = assets.find(id);
    if (found == assets.end()) {
        diagnostics.push_back(Error("PXRUNTIME7317", "Runtime IR references an unknown asset UUID", sourcePath, line, property, idText, "asset:" + idText));
        return std::nullopt;
    }
    if (!exists || !exists(found->second.path)) {
        diagnostics.push_back(Error("PXRUNTIME7318", "Runtime IR asset file is missing", sourcePath, line, property, idText, found->second.path));
        return std::nullopt;
    }
    return ResourceRefValue{ found->second.id, found->second.path };
}

std::optional<ResourceRefValue> ResolveToken(
    const std::string_view token, const ProjectAssets& assets, const RuntimeAssetExists& exists, const std::string& sourcePath, const int line, const std::string& property, std::vector<diag::Diagnostic>& diagnostics
) {
    const std::string_view idText = token.substr(kAssetPrefix.size());
    const auto id = Uuid::Parse(idText);
    if (!id) {
        diagnostics.push_back(Error("PXRUNTIME7316", "Runtime IR asset reference must use asset:<uuid>", sourcePath, line, property, std::string(idText), std::string(token)));
        return std::nullopt;
    }
    return ResolveId(*id, assets, exists, sourcePath, line, property, diagnostics);
}

void ResolveVariant(Variant& value, const ProjectAssets& assets, const RuntimeAssetExists& exists, const std::string& sourcePath, const int line, const std::string& property, std::vector<diag::Diagnostic>& diagnostics) {
    if (const auto* text = value.TryGet<std::string>(); text && IsAssetToken(*text)) {
        if (auto resolved = ResolveToken(*text, assets, exists, sourcePath, line, property, diagnostics)) {
            value = Variant(std::move(*resolved));
        }
        return;
    }
    if (const auto* reference = value.TryGet<ResourceRefValue>()) {
        std::optional<ResourceRefValue> resolved;
        if (!reference->id.Empty()) {
            resolved = ResolveId(reference->id, assets, exists, sourcePath, line, property, diagnostics);
        }
        else if (IsAssetToken(reference->lastKnownPath)) {
            resolved = ResolveToken(reference->lastKnownPath, assets, exists, sourcePath, line, property, diagnostics);
        }
        if (resolved) value = Variant(std::move(*resolved));
        return;
    }
    if (auto* array = value.AsArray()) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            ResolveVariant((*array)[index], assets, exists, sourcePath, line, property + "[" + std::to_string(index) + "]", diagnostics);
        }
        return;
    }
    if (auto* object = value.AsObject()) {
        for (auto& [name, field] : *object) {
            ResolveVariant(field, assets, exists, sourcePath, line, property.empty() ? name : property + "." + name, diagnostics);
        }
    }
}

}  // namespace

bool UsesRuntimeAssetReferences(const vn::Program& program) {
    for (const auto& command : program.code) {
        if (std::any_of(command.args.begin(), command.args.end(), [](const vn::Arg& argument) { return IsAssetToken(argument.value); })) {
            return true;
        }
        if (std::any_of(command.typedArgs.begin(), command.typedArgs.end(), [](const auto& field) { return UsesAssetReference(field.second); })) {
            return true;
        }
    }
    return false;
}

Result<vn::Program> ResolveRuntimeAssetReferences(vn::Program program, const std::string_view projectManifest, const RuntimeAssetExists& exists, const std::string& sourcePath) {
    if (!UsesRuntimeAssetReferences(program)) {
        return Result<vn::Program>::Success(std::move(program));
    }
    auto parsedAssets = ParseProjectAssets(projectManifest);
    if (!parsedAssets) {
        return Result<vn::Program>::Failure(parsedAssets.Diagnostics());
    }
    const ProjectAssets& assets = parsedAssets.Value();
    std::vector<diag::Diagnostic> diagnostics;
    for (auto& command : program.code) {
        for (auto& argument : command.args) {
            if (!IsAssetToken(argument.value)) continue;
            if (auto resolved = ResolveToken(argument.value, assets, exists, sourcePath, command.line, argument.key, diagnostics)) {
                argument.value = resolved->lastKnownPath;
                command.typedArgs[argument.key] = Variant(std::move(*resolved));
            }
        }
        for (auto& [name, value] : command.typedArgs) {
            ResolveVariant(value, assets, exists, sourcePath, command.line, name, diagnostics);
            if (const auto* reference = value.TryGet<ResourceRefValue>()) {
                auto argument = std::find_if(command.args.begin(), command.args.end(), [&name](const vn::Arg& candidate) { return candidate.key == name; });
                if (argument == command.args.end()) {
                    command.args.push_back({ name, reference->lastKnownPath });
                }
                else {
                    argument->value = reference->lastKnownPath;
                }
            }
        }
    }
    if (!diagnostics.empty()) {
        return Result<vn::Program>::Failure(std::move(diagnostics));
    }
    return Result<vn::Program>::Success(std::move(program));
}

}  // namespace px
