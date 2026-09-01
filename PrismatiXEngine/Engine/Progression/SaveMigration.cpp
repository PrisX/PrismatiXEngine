#include "Engine/Progression/SaveSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <unordered_set>

namespace px::progress {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxMigrationBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxMigrationOperations = 100'000;
constexpr std::size_t kMaxMigrationSteps = 64;

diag::Diagnostic MigrationError(std::string code, std::string message,
                                const std::string& asset = {},
                                std::string details = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "Persistence.SaveMigration",
                                .message = std::move(message),
                                .details = std::move(details)};
    diagnostic.source.path = asset;
    return diagnostic;
}

bool SameVersion(const std::string_view leftContent,
                 const std::uint32_t leftSave,
                 const std::string_view rightContent,
                 const std::uint32_t rightSave) {
    return leftContent == rightContent && leftSave == rightSave;
}

std::optional<vn::Value> JsonValue(const Json& source, const int depth = 0) {
    if (depth > 64) return std::nullopt;
    if (source.is_null()) return vn::Value{};
    if (source.is_boolean()) return vn::Value(source.get<bool>());
    if (source.is_number_integer()) return vn::Value(source.get<std::int64_t>());
    if (source.is_number_unsigned()) {
        const auto value = source.get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int64_t>::max)()))
            return std::nullopt;
        return vn::Value(static_cast<std::int64_t>(value));
    }
    if (source.is_number_float()) return vn::Value(source.get<double>());
    if (source.is_string()) return vn::Value(source.get<std::string>());
    if (source.is_array()) {
        if (source.size() > kMaxMigrationOperations) return std::nullopt;
        vn::ValueList values;
        values.reserve(source.size());
        for (const auto& item : source) {
            auto value = JsonValue(item, depth + 1);
            if (!value) return std::nullopt;
            values.push_back(std::move(*value));
        }
        return vn::Value(std::move(values));
    }
    if (source.is_object()) {
        if (source.size() > kMaxMigrationOperations) return std::nullopt;
        vn::ValueMap values;
        for (auto item = source.begin(); item != source.end(); ++item) {
            auto value = JsonValue(item.value(), depth + 1);
            if (!value) return std::nullopt;
            values.emplace(item.key(), std::move(*value));
        }
        return vn::Value(std::move(values));
    }
    return std::nullopt;
}

bool ReadAnchor(const Json& value, ExecutionAnchor& anchor) {
    if (!value.is_object()) return false;
    anchor.runtimeDocumentId = value.value("runtimeDocumentId", std::string{});
    anchor.sourceId = value.value("sourceId", std::string{});
    anchor.operationId = value.value("operationId", std::string{});
    return !anchor.runtimeDocumentId.empty() && !anchor.sourceId.empty() &&
           !anchor.operationId.empty();
}

bool SameAnchor(const ExecutionAnchor& left, const ExecutionAnchor& right) {
    return left.runtimeDocumentId == right.runtimeDocumentId &&
           left.sourceId == right.sourceId &&
           left.operationId == right.operationId;
}

Status ApplyMigrationDocument(SaveSnapshot& candidate,
                              const SaveMigrationDescriptor& descriptor,
                              const std::string_view text) {
    if (text.empty() || text.size() > kMaxMigrationBytes) {
        return Status::Fail(MigrationError(
            "PXSAVE6204", "Save migration is empty or exceeds 4 MiB",
            descriptor.asset));
    }
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return Status::Fail(MigrationError(
            "PXSAVE6205", "Save migration is not valid JSON",
            descriptor.asset));
    }
    try {
        if (root.value("format", std::string{}) != "PrismatiXSaveMigration" ||
            root.value("schemaRevision", 0) != 2 ||
            root.value("id", std::string{}) != descriptor.id) {
            return Status::Fail(MigrationError(
                "PXSAVE6206", "Save migration format, revision, or identity is invalid",
                descriptor.asset));
        }
        const auto& from = root.at("from");
        const auto& to = root.at("to");
        if (!from.is_object() || !to.is_object() ||
            from.value("contentVersion", std::string{}) !=
                descriptor.fromContentVersion ||
            from.value("saveVersion", std::uint32_t{0}) !=
                descriptor.fromSaveVersion ||
            to.value("contentVersion", std::string{}) !=
                descriptor.toContentVersion ||
            to.value("saveVersion", std::uint32_t{0}) !=
                descriptor.toSaveVersion) {
            return Status::Fail(MigrationError(
                "PXSAVE6207", "Save migration endpoints do not match the package manifest",
                descriptor.asset));
        }
        const auto& anchor = root.at("anchor");
        if (!anchor.is_object()) {
            return Status::Fail(MigrationError(
                "PXSAVE6208", "Save migration must declare an anchor policy",
                descriptor.asset));
        }
        const std::string anchorPolicy =
            anchor.value("policy", std::string{});
        if (anchorPolicy == "map") {
            const auto& mappings = anchor.at("mappings");
            if (!mappings.is_array() || mappings.empty() ||
                mappings.size() > kMaxMigrationOperations) {
                return Status::Fail(MigrationError(
                    "PXSAVE6208", "Mapped anchor policy requires a bounded mappings array",
                    descriptor.asset));
            }
            bool mapped = false;
            for (const auto& mapping : mappings) {
                if (!mapping.is_object()) continue;
                ExecutionAnchor fromAnchor;
                ExecutionAnchor toAnchor;
                if (!ReadAnchor(mapping.value("from", Json{}), fromAnchor) ||
                    !ReadAnchor(mapping.value("to", Json{}), toAnchor)) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6208", "Save migration contains an invalid anchor mapping",
                        descriptor.asset));
                }
                if (SameAnchor(candidate.anchor, fromAnchor)) {
                    if (mapped) {
                        return Status::Fail(MigrationError(
                            "PXSAVE6209", "Save anchor maps to more than one destination",
                            descriptor.asset));
                    }
                    candidate.anchor = std::move(toAnchor);
                    mapped = true;
                }
            }
            if (!mapped) {
                return Status::Fail(MigrationError(
                    "PXSAVE6210", "Save migration has no mapping for the execution anchor",
                    descriptor.asset));
            }
        } else if (anchorPolicy != "preserve") {
            return Status::Fail(MigrationError(
                "PXSAVE6208", "Save migration anchor policy must be preserve or map",
                descriptor.asset));
        }

        const auto operations = root.value("operations", Json::array());
        if (!operations.is_array() ||
            operations.size() > kMaxMigrationOperations) {
            return Status::Fail(MigrationError(
                "PXSAVE6211", "Save migration operations must be a bounded array",
                descriptor.asset));
        }
        for (const auto& operation : operations) {
            if (!operation.is_object()) {
                return Status::Fail(MigrationError(
                    "PXSAVE6212", "Save migration operation must be an object",
                    descriptor.asset));
            }
            const std::string op = operation.value("op", std::string{});
            if (op == "renameVariable") {
                const std::string fromName =
                    operation.value("from", std::string{});
                const std::string toName = operation.value("to", std::string{});
                if (fromName.empty() || toName.empty() ||
                    candidate.typedVariables.contains(toName) ||
                    candidate.variables.contains(toName)) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6213", "renameVariable source/destination is invalid or collides",
                        descriptor.asset));
                }
                if (auto found = candidate.typedVariables.find(fromName);
                    found != candidate.typedVariables.end()) {
                    candidate.typedVariables.emplace(toName,
                                                     found->second.Clone());
                    candidate.typedVariables.erase(found);
                }
                if (auto found = candidate.variables.find(fromName);
                    found != candidate.variables.end()) {
                    candidate.variables.emplace(toName, found->second);
                    candidate.variables.erase(found);
                }
            } else if (op == "removeVariable") {
                const std::string name = operation.value("name", std::string{});
                if (name.empty()) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6214", "removeVariable requires a name",
                        descriptor.asset));
                }
                candidate.typedVariables.erase(name);
                candidate.variables.erase(name);
            } else if (op == "setVariable") {
                const std::string name = operation.value("name", std::string{});
                if (name.empty() || !operation.contains("value")) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6215", "setVariable requires name and value",
                        descriptor.asset));
                }
                auto value = JsonValue(operation["value"]);
                if (!value) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6215", "setVariable value is unsupported or exceeds limits",
                        descriptor.asset));
                }
                candidate.typedVariables[name] = value->Clone();
                if (const auto* integer = value->TryGet<std::int64_t>();
                    integer && *integer >= (std::numeric_limits<int>::min)() &&
                    *integer <= (std::numeric_limits<int>::max)())
                    candidate.variables[name] = static_cast<int>(*integer);
                else
                    candidate.variables.erase(name);
            } else if (op == "renameRoute") {
                const std::string fromRoute =
                    operation.value("from", std::string{});
                const std::string toRoute = operation.value("to", std::string{});
                if (fromRoute.empty() || toRoute.empty()) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6216", "renameRoute requires from and to",
                        descriptor.asset));
                }
                for (auto& route : candidate.routes.stack)
                    if (route == fromRoute) route = toRoute;
                for (auto& route : candidate.routes.modals)
                    if (route == fromRoute) route = toRoute;
            } else if (op == "setScript") {
                const std::string path = operation.value("path", std::string{});
                if (path.empty()) {
                    return Status::Fail(MigrationError(
                        "PXSAVE6217", "setScript requires a path",
                        descriptor.asset));
                }
                candidate.scriptPath = path;
                candidate.vm.scriptPath = path;
            } else {
                return Status::Fail(MigrationError(
                    "PXSAVE6218", "Unknown save migration operation: " + op,
                    descriptor.asset));
            }
        }
        candidate.contentVersion = descriptor.toContentVersion;
        candidate.saveVersion = descriptor.toSaveVersion;
        return Status::Ok();
    } catch (const Json::exception& error) {
        return Status::Fail(MigrationError(
            "PXSAVE6219", "Save migration contains invalid field types",
            descriptor.asset, error.what()));
    }
}

}  // namespace

Result<SaveSnapshot> MigrateSaveSnapshot(
    const SaveSnapshot& source, const SaveMigrationTarget& target,
    const std::vector<SaveMigrationDescriptor>& migrations,
    const SaveMigrationReadText& readText) {
    if (source.gameId != target.gameId || target.gameId.empty() ||
        target.packageFingerprint.empty() || target.contentVersion.empty() ||
        target.saveVersion == 0) {
        return Result<SaveSnapshot>::Failure(MigrationError(
            "PXSAVE6200", "Save and target package identities are incompatible"));
    }
    SaveSnapshot candidate = source;
    std::unordered_set<std::string> visited;
    std::size_t stepCount = 0;
    while (!SameVersion(candidate.contentVersion, candidate.saveVersion,
                        target.contentVersion, target.saveVersion)) {
        if (++stepCount > kMaxMigrationSteps) {
            return Result<SaveSnapshot>::Failure(MigrationError(
                "PXSAVE6201", "Save migration chain exceeds 64 steps"));
        }
        const SaveMigrationDescriptor* selected = nullptr;
        for (const auto& migration : migrations) {
            if (!SameVersion(candidate.contentVersion, candidate.saveVersion,
                             migration.fromContentVersion,
                             migration.fromSaveVersion))
                continue;
            if (selected) {
                return Result<SaveSnapshot>::Failure(MigrationError(
                    "PXSAVE6202", "Save migration chain is ambiguous"));
            }
            selected = &migration;
        }
        if (!selected) {
            return Result<SaveSnapshot>::Failure(MigrationError(
                "PXSAVE6203", "No explicit save migration reaches the current package"));
        }
        if (selected->id.empty() || selected->asset.empty() ||
            selected->toContentVersion.empty() || selected->toSaveVersion == 0 ||
            !visited.insert(selected->id).second) {
            return Result<SaveSnapshot>::Failure(MigrationError(
                "PXSAVE6201", "Save migration chain contains an invalid step or cycle",
                selected->asset));
        }
        const auto text = readText ? readText(selected->asset) : std::nullopt;
        if (!text) {
            return Result<SaveSnapshot>::Failure(MigrationError(
                "PXSAVE6204", "Declared save migration asset is unavailable",
                selected->asset));
        }
        const Status applied =
            ApplyMigrationDocument(candidate, *selected, *text);
        if (!applied) {
            return Result<SaveSnapshot>::Failure(applied.Diagnostics());
        }
    }
    candidate.engineVersion = "0.2.0";
    candidate.packageFingerprint = target.packageFingerprint;
    return Result<SaveSnapshot>::Success(std::move(candidate));
}

}  // namespace px::progress
