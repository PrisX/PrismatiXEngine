#include "Engine/Package/PackageManifest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>

namespace px::sdk::detail {
namespace {

using Json = nlohmann::json;

void AddDiagnostic(std::vector<PackageDiagnostic>& diagnostics,
                   std::string code, std::string message) {
    diagnostics.push_back({std::move(code), std::move(message), false});
}

std::optional<int> JsonInt(const Json& value) {
    if (!value.is_number_integer()) return std::nullopt;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))
            return std::nullopt;
        return static_cast<int>(number);
    }
    const auto number = value.get<std::int64_t>();
    if (number < (std::numeric_limits<int>::min)() ||
        number > (std::numeric_limits<int>::max)())
        return std::nullopt;
    return static_cast<int>(number);
}

bool IsIdentifier(const std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' ||
                      character == '_' || character == '.';
           });
}

bool IsFingerprint(const std::string_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

bool IsSafeUri(const std::string_view uri) {
    if (uri.empty() || uri.size() > 4096 || uri.front() == '/' ||
        uri.find('\\') != std::string_view::npos ||
        uri.find(':') != std::string_view::npos ||
        uri.find('\0') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start < uri.size()) {
        const auto end = uri.find('/', start);
        const auto component = uri.substr(
            start, end == std::string_view::npos ? uri.size() - start
                                                  : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::optional<std::array<float, 4>> EffectDefault(
    const Json& value, const std::string_view type) {
    std::array<float, 4> result{};
    const auto finite = [](const Json& item, float& output) {
        if (!item.is_number()) return false;
        const double number = item.get<double>();
        if (!std::isfinite(number) ||
            number < -(std::numeric_limits<float>::max)() ||
            number > (std::numeric_limits<float>::max)())
            return false;
        output = static_cast<float>(number);
        return true;
    };
    if (type == "number")
        return finite(value, result[0])
                   ? std::optional<std::array<float, 4>>(result)
                   : std::nullopt;
    const std::size_t required = type == "vec2" ? 2 : type == "color" ? 4 : 0;
    if (required == 0 || !value.is_array() || value.size() != required)
        return std::nullopt;
    for (std::size_t index = 0; index < required; ++index) {
        if (!finite(value[index], result[index])) return std::nullopt;
        if (type == "color" && (result[index] < 0.0f || result[index] > 1.0f))
            return std::nullopt;
    }
    return result;
}

}  // namespace

PackageManifestParseResult ParsePackageManifest(const std::string_view text) {
    PackageManifestParseResult result;
    if (text.empty() || text.size() > 4 * 1024 * 1024) {
        AddDiagnostic(result.diagnostics, "PXPKG1400",
                      "Package manifest must be between 1 byte and 4 MiB");
        return result;
    }
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result.diagnostics, "PXPKG1401",
                      "Package manifest must be a JSON object");
        return result;
    }
    static const std::set<std::string> allowed{
        "format", "schemaRevision", "engineVersion", "gameId", "title",
        "resolution", "entry", "routes", "archives", "contentVersion",
        "saveVersion", "packageFingerprint", "encryption", "graphicsTier",
        "saveMigrations", "extensions", "customEffects"};
    for (auto item = root.begin(); item != root.end(); ++item) {
        if (!allowed.contains(item.key()))
            AddDiagnostic(result.diagnostics, "PXPKG1402",
                          "Unknown package manifest field: " + item.key());
    }

    try {
        if (root.value("format", std::string{}) != "PrismatiXPackageManifest" ||
            root.value("schemaRevision", 0) != 2)
            AddDiagnostic(result.diagnostics, "PXPKG1403",
                          "Package manifest format or schema revision is unsupported");

        auto& manifest = result.manifest;
        manifest.engineVersion = root.value("engineVersion", std::string{});
        manifest.gameId = root.value("gameId", std::string{});
        manifest.title = root.value("title", std::string{});
        manifest.contentVersion = root.value("contentVersion", std::string{});
        manifest.packageFingerprint =
            root.value("packageFingerprint", std::string{});
        manifest.graphicsTier = root.value("graphicsTier", std::string{});
        const auto saveVersion = root.contains("saveVersion")
                                     ? JsonInt(root["saveVersion"])
                                     : std::optional<int>{};
        if (saveVersion && *saveVersion > 0)
            manifest.saveVersion = static_cast<std::uint32_t>(*saveVersion);
        else
            AddDiagnostic(result.diagnostics, "PXPKG1404",
                          "saveVersion is invalid");
        if (manifest.engineVersion != "0.2.0" ||
            !IsIdentifier(manifest.gameId) || manifest.title.empty() ||
            manifest.contentVersion.empty() ||
            !IsFingerprint(manifest.packageFingerprint) ||
            (manifest.graphicsTier != "basic" &&
             manifest.graphicsTier != "gpu-effects"))
            AddDiagnostic(result.diagnostics, "PXPKG1405",
                          "Package identity, version, title, or graphics tier is invalid");

        const auto resolution = root.find("resolution");
        const auto entry = root.find("entry");
        const auto encryption = root.find("encryption");
        if (resolution == root.end() || !resolution->is_object() ||
            entry == root.end() || !entry->is_object() ||
            encryption == root.end() || !encryption->is_object()) {
            AddDiagnostic(result.diagnostics, "PXPKG1406",
                          "resolution, entry, and encryption objects are required");
        } else {
            manifest.width = resolution->value("width", 0);
            manifest.height = resolution->value("height", 0);
            manifest.startRuntimeIr = entry->value("runtimeIr", std::string{});
            manifest.sourceMap = entry->value("sourceMap", std::string{});
            manifest.startRoute = entry->value("route", std::string{});
            manifest.encrypted = encryption->value("enabled", false);
            manifest.archiveKey =
                encryption->value("archiveKey", std::string{});
            if (manifest.width < 320 || manifest.width > 16384 ||
                manifest.height < 180 || manifest.height > 16384 ||
                !IsSafeUri(manifest.startRuntimeIr) ||
                !manifest.startRuntimeIr.ends_with(".pxir") ||
                !IsSafeUri(manifest.sourceMap) ||
                !manifest.sourceMap.ends_with(".pxmap") ||
                (manifest.encrypted && manifest.archiveKey.empty()) ||
                (!manifest.encrypted && !manifest.archiveKey.empty()))
                AddDiagnostic(result.diagnostics, "PXPKG1407",
                              "Package resolution, entry Runtime IR, or encryption state is invalid");
        }

        const auto routes = root.find("routes");
        if (routes == root.end() || !routes->is_array() ||
            routes->size() > 1024) {
            AddDiagnostic(result.diagnostics, "PXPKG1408",
                          "routes must be a bounded array");
        } else {
            std::set<std::string> ids;
            for (const auto& value : *routes) {
                if (!value.is_object()) {
                    AddDiagnostic(result.diagnostics, "PXPKG1408",
                                  "route must be an object");
                    continue;
                }
                PackageManifestRoute route{
                    value.value("id", std::string{}),
                    value.value("sceneId", std::string{}),
                    value.value("scene", std::string{})};
                if (!IsIdentifier(route.id) || route.sceneId.empty() ||
                    !IsSafeUri(route.scene) || !ids.insert(route.id).second) {
                    AddDiagnostic(result.diagnostics, "PXPKG1409",
                                  "Package route identity or scene path is invalid");
                    continue;
                }
                manifest.routes.push_back(std::move(route));
            }
            if (!manifest.startRoute.empty() &&
                !ids.contains(manifest.startRoute))
                AddDiagnostic(result.diagnostics, "PXPKG1410",
                              "Package entry route does not exist");
        }

        const auto archives = root.find("archives");
        if (archives == root.end() || !archives->is_array() ||
            archives->empty() || archives->size() > 64) {
            AddDiagnostic(result.diagnostics, "PXPKG1411",
                          "archives must be a non-empty bounded array");
        } else {
            std::set<std::string> files;
            for (const auto& value : *archives) {
                if (!value.is_object()) {
                    AddDiagnostic(result.diagnostics, "PXPKG1411",
                                  "archive must be an object");
                    continue;
                }
                PackageArchiveMount archive{
                    value.value("file", std::string{}),
                    value.value("group", std::string{}),
                    value.value("optional", false)};
                if (!IsSafeUri(archive.file) || archive.group.empty() ||
                    !files.insert(archive.file).second) {
                    AddDiagnostic(result.diagnostics, "PXPKG1412",
                                  "Package archive mount is invalid or duplicated");
                    continue;
                }
                manifest.archives.push_back(std::move(archive));
            }
        }

        const auto migrations = root.find("saveMigrations");
        if (migrations == root.end() || !migrations->is_array() ||
            migrations->size() > 64) {
            AddDiagnostic(result.diagnostics, "PXPKG1414",
                          "saveMigrations must be a bounded array");
        } else {
            std::set<std::string> ids;
            std::set<std::pair<std::string, std::uint32_t>> sources;
            for (const auto& value : *migrations) {
                if (!value.is_object() || !value.contains("from") ||
                    !value["from"].is_object() || !value.contains("to") ||
                    !value["to"].is_object()) {
                    AddDiagnostic(result.diagnostics, "PXPKG1415",
                                  "save migration must contain from and to objects");
                    continue;
                }
                PackageSaveMigration migration;
                migration.id = value.value("id", std::string{});
                migration.asset = value.value("asset", std::string{});
                migration.fromContentVersion =
                    value["from"].value("contentVersion", std::string{});
                migration.fromSaveVersion =
                    value["from"].value("saveVersion", std::uint32_t{0});
                migration.toContentVersion =
                    value["to"].value("contentVersion", std::string{});
                migration.toSaveVersion =
                    value["to"].value("saveVersion", std::uint32_t{0});
                if (!IsIdentifier(migration.id) ||
                    migration.fromContentVersion.empty() ||
                    migration.toContentVersion.empty() ||
                    migration.fromSaveVersion == 0 ||
                    migration.toSaveVersion == 0 ||
                    !IsSafeUri(migration.asset) ||
                    !ids.insert(migration.id).second ||
                    !sources.insert({migration.fromContentVersion,
                                     migration.fromSaveVersion}).second) {
                    AddDiagnostic(result.diagnostics, "PXPKG1416",
                                  "save migration identity, endpoints, or asset is invalid or ambiguous");
                    continue;
                }
                manifest.saveMigrations.push_back(std::move(migration));
            }
        }

        const auto extensions = root.find("extensions");
        if (extensions == root.end() || !extensions->is_array() ||
            extensions->size() > 1024) {
            AddDiagnostic(result.diagnostics, "PXPKG1417",
                          "extensions must be a bounded array");
        } else {
            std::set<std::string> paths;
            for (const auto& value : *extensions) {
                if (!value.is_string() ||
                    !IsSafeUri(value.get<std::string>()) ||
                    !value.get<std::string>().ends_with(".pxextension") ||
                    !paths.insert(value.get<std::string>()).second) {
                    AddDiagnostic(result.diagnostics, "PXPKG1418",
                                  "extension path is unsafe or duplicated");
                    continue;
                }
                manifest.extensions.push_back(value.get<std::string>());
            }
        }

        const auto customEffects = root.find("customEffects");
        if (customEffects == root.end() || !customEffects->is_array() ||
            customEffects->size() > 64) {
            AddDiagnostic(result.diagnostics, "PXPKG1419",
                          "customEffects must be a bounded array");
        } else {
            std::set<std::string> effectIds;
            for (const Json& value : *customEffects) {
                if (!value.is_object() || value.size() != 5 ||
                    !value.contains("id") || !value["id"].is_string() ||
                    !value.contains("targetLayer") ||
                    !value["targetLayer"].is_string() ||
                    !value.contains("uniforms") ||
                    !value["uniforms"].is_array() ||
                    !value.contains("artifacts") ||
                    !value["artifacts"].is_array() ||
                    !value.contains("reflection") ||
                    !value["reflection"].is_object()) {
                    AddDiagnostic(result.diagnostics, "PXPKG1420",
                                  "Custom effect package descriptor is invalid");
                    continue;
                }
                PackageCustomEffect effect;
                effect.id = value["id"].get<std::string>();
                effect.targetLayer = value["targetLayer"].get<std::string>();
                if (!IsIdentifier(effect.id) || effect.targetLayer != "stage" ||
                    !effectIds.insert(effect.id).second ||
                    value["uniforms"].size() > 8 ||
                    value["artifacts"].size() != 3) {
                    AddDiagnostic(result.diagnostics, "PXPKG1421",
                                  "Custom effect identity, layer, or bounds are invalid");
                    continue;
                }
                bool valid = true;
                std::set<std::string> uniformNames;
                std::set<std::uint32_t> uniformSlots;
                for (const Json& uniformValue : value["uniforms"]) {
                    if (!uniformValue.is_object() ||
                        !uniformValue.contains("name") ||
                        !uniformValue["name"].is_string() ||
                        !uniformValue.contains("type") ||
                        !uniformValue["type"].is_string() ||
                        !uniformValue.contains("slot") ||
                        !uniformValue["slot"].is_number_unsigned() ||
                        !uniformValue.contains("default") ||
                        !uniformValue.contains("minimum") ||
                        !uniformValue["minimum"].is_number() ||
                        !uniformValue.contains("maximum") ||
                        !uniformValue["maximum"].is_number()) {
                        valid = false;
                        break;
                    }
                    PackageEffectUniform uniform;
                    uniform.name = uniformValue["name"].get<std::string>();
                    uniform.type = uniformValue["type"].get<std::string>();
                    uniform.slot = uniformValue["slot"].get<std::uint32_t>();
                    const auto defaultValue =
                        EffectDefault(uniformValue["default"], uniform.type);
                    const double minimum =
                        uniformValue["minimum"].get<double>();
                    const double maximum =
                        uniformValue["maximum"].get<double>();
                    const double floatLimit = static_cast<double>(
                        (std::numeric_limits<float>::max)());
                    const std::size_t components = uniform.type == "number" ? 1u
                                                 : uniform.type == "vec2" ? 2u
                                                 : uniform.type == "color" ? 4u : 0u;
                    bool defaultInRange = defaultValue && components > 0;
                    if (defaultInRange) {
                        for (std::size_t component = 0;
                             component < components; ++component) {
                            const double number = (*defaultValue)[component];
                            defaultInRange = defaultInRange &&
                                             number >= minimum &&
                                             number <= maximum;
                        }
                    }
                    if (!IsIdentifier(uniform.name) || uniform.slot > 7 ||
                        !defaultValue || !std::isfinite(minimum) ||
                        !std::isfinite(maximum) ||
                        std::abs(minimum) > floatLimit ||
                        std::abs(maximum) > floatLimit || minimum > maximum ||
                        !defaultInRange ||
                        !uniformNames.insert(uniform.name).second ||
                        !uniformSlots.insert(uniform.slot).second) {
                        valid = false;
                        break;
                    }
                    uniform.defaultValue = *defaultValue;
                    uniform.minimum = static_cast<float>(minimum);
                    uniform.maximum = static_cast<float>(maximum);
                    effect.uniforms.push_back(std::move(uniform));
                }
                std::set<std::string> formats;
                for (const Json& artifactValue : value["artifacts"]) {
                    if (!artifactValue.is_object()) {
                        valid = false;
                        break;
                    }
                    PackageShaderArtifact artifact{
                        artifactValue.value("format", std::string{}),
                        artifactValue.value("asset", std::string{}),
                        artifactValue.value("fingerprint", std::string{})};
                    if ((artifact.format != "spirv" &&
                         artifact.format != "dxil" &&
                         artifact.format != "msl") ||
                        !formats.insert(artifact.format).second ||
                        !IsSafeUri(artifact.asset) ||
                        !IsFingerprint(artifact.fingerprint)) {
                        valid = false;
                        break;
                    }
                    effect.artifacts.push_back(std::move(artifact));
                }
                const Json& reflection = value["reflection"];
                if (!reflection.contains("samplers") ||
                    !reflection["samplers"].is_number_unsigned() ||
                    !reflection.contains("uniformBuffers") ||
                    !reflection["uniformBuffers"].is_number_unsigned() ||
                    reflection.value("storageTextures", 1u) != 0 ||
                    reflection.value("storageBuffers", 1u) != 0) {
                    valid = false;
                } else {
                    effect.samplerCount =
                        reflection["samplers"].get<std::uint32_t>();
                    effect.uniformBufferCount =
                        reflection["uniformBuffers"].get<std::uint32_t>();
                    valid = valid && effect.samplerCount == 1 &&
                            effect.uniformBufferCount == 1;
                }
                if (formats != std::set<std::string>{"dxil", "msl", "spirv"})
                    valid = false;
                if (!valid) {
                    AddDiagnostic(result.diagnostics, "PXPKG1422",
                                  "Custom effect uniforms, reflection, or artifacts are invalid: " +
                                      effect.id);
                    continue;
                }
                manifest.customEffects.push_back(std::move(effect));
            }
            if (!manifest.customEffects.empty() &&
                manifest.graphicsTier != "gpu-effects")
                AddDiagnostic(result.diagnostics, "PXPKG1423",
                              "Custom effects require the gpu-effects tier");
        }
    } catch (const Json::exception& error) {
        AddDiagnostic(result.diagnostics, "PXPKG1413",
                      std::string("Package manifest field has the wrong type: ") +
                          error.what());
    }
    return result;
}

}  // namespace px::sdk::detail
