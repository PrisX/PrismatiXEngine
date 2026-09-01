#include "Engine/Preview/ProgressionPreview.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace px::preview {
namespace {

using Json = nlohmann::json;

diag::Diagnostic Error(std::string code, std::string message,
                       const std::string& documentId = {},
                       const std::string& nodeId = {},
                       std::string property = {}) {
    diag::Diagnostic diagnostic;
    diagnostic.severity = diag::Severity::Error;
    diagnostic.code = std::move(code);
    diagnostic.category = "Preview.Progression";
    diagnostic.message = std::move(message);
    diagnostic.source.resourceId = documentId;
    diagnostic.source.nodeId = nodeId;
    diagnostic.source.property = std::move(property);
    return diagnostic;
}

bool IsLiteral(const Json& value) {
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        return false;
    return value.is_null() || value.is_boolean() || value.is_number() ||
           value.is_string();
}

bool IsIdentifier(const std::string_view value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (first != '_' && !std::isalpha(first)) return false;
    return std::ranges::all_of(value.substr(1), [](const unsigned char item) {
        return item == '_' || std::isalnum(item);
    });
}

bool IsScopedVariable(const std::string_view value) {
    const auto separator = value.find('.');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= value.size())
        return false;
    const auto scope = value.substr(0, separator);
    return (scope == "local" || scope == "run" || scope == "session" ||
            scope == "profile" || scope == "device" || scope == "account") &&
           IsIdentifier(value.substr(separator + 1));
}

bool IsVariableScope(const std::string_view value) {
    return value == "local" || value == "run" || value == "session" ||
           value == "profile" || value == "device" || value == "account";
}

bool IsBlank(const std::string_view value) {
    return std::ranges::all_of(value, [](const unsigned char item) {
        return std::isspace(item) != 0;
    });
}

bool CompareNumber(const double actual, const std::string_view operation,
                   const Json& expectedValue) {
    if (!expectedValue.is_number()) return false;
    const double expected = expectedValue.get<double>();
    if (operation == "eq") return actual == expected;
    if (operation == "ne") return actual != expected;
    if (operation == "gt") return actual > expected;
    if (operation == "gte") return actual >= expected;
    if (operation == "lt") return actual < expected;
    if (operation == "lte") return actual <= expected;
    return false;
}

bool CompareBoolean(const bool actual, const std::string_view operation,
                    const Json& expectedValue) {
    if (!expectedValue.is_boolean()) return false;
    const bool expected = expectedValue.get<bool>();
    if (operation == "is") return actual == expected;
    if (operation == "isNot") return actual != expected;
    return false;
}

bool RequirementMet(const Json& requirement, const Json& state,
                    const std::unordered_set<std::string>& clearedRoutes,
                    const std::unordered_set<std::string>& flags) {
    const auto kind = requirement["kind"].get<std::string>();
    const auto key = requirement["key"].get<std::string>();
    const auto operation = requirement["operator"].get<std::string>();
    const Json& expected = requirement["value"];
    if (kind == "routeCleared")
        return CompareBoolean(clearedRoutes.contains(key), operation, expected);
    if (kind == "flag")
        return CompareBoolean(flags.contains(key), operation, expected);
    if (kind == "clearCount")
        return CompareNumber(state["clearCount"].get<double>(), operation,
                             expected);
    const auto found = state["variables"].find(key);
    if (found == state["variables"].end()) return false;
    if (operation == "eq") return *found == expected;
    if (operation == "ne") return *found != expected;
    if (found->is_number())
        return CompareNumber(found->get<double>(), operation, expected);
    return false;
}

std::optional<diag::Diagnostic> ValidateRequirement(
    const Json& requirement, const std::string& documentId) {
    if (!requirement.is_object() ||
        !requirement.contains("id") || !requirement["id"].is_string() ||
        !requirement.contains("kind") || !requirement["kind"].is_string() ||
        !requirement.contains("key") || !requirement["key"].is_string() ||
        !requirement.contains("operator") ||
        !requirement["operator"].is_string() ||
        !requirement.contains("value") || !IsLiteral(requirement["value"])) {
        return Error("PXWASM-PROGRESSION-REQUIREMENT-001",
                     "Progression requirements require stable identity, kind, key, operator, and a literal value.",
                     documentId);
    }
    const auto id = requirement["id"].get<std::string>();
    const auto kind = requirement["kind"].get<std::string>();
    const auto key = requirement["key"].get<std::string>();
    const auto operation = requirement["operator"].get<std::string>();
    if (id.empty() || key.empty()) {
        return Error("PXWASM-PROGRESSION-REQUIREMENT-002",
                     "Progression requirement identities and keys cannot be empty.",
                     documentId, id, "requirements");
    }
    const auto numericOperation = operation == "eq" || operation == "ne" ||
                                  operation == "gt" || operation == "gte" ||
                                  operation == "lt" || operation == "lte";
    bool valid = false;
    if (kind == "routeCleared")
        valid = (operation == "is" || operation == "isNot") &&
                requirement["value"].is_boolean();
    else if (kind == "flag")
        valid = IsIdentifier(key) &&
                (operation == "is" || operation == "isNot") &&
                requirement["value"].is_boolean();
    else if (kind == "clearCount")
        valid = key == "clearCount" && numericOperation &&
                requirement["value"].is_number();
    else if (kind == "variable")
        valid = IsScopedVariable(key) && numericOperation;
    if (!valid) {
        return Error("PXWASM-PROGRESSION-REQUIREMENT-003",
                     "Progression requirement kind, operator, key, or value is incompatible.",
                     documentId, id, "requirements");
    }
    return std::nullopt;
}

}  // namespace

Result<ProgressionPreviewSimulation> SimulateProgressionPreview(
    const std::string_view requestJson) {
    const Json request = Json::parse(requestJson, nullptr, false);
    if (request.is_discarded() || !request.is_object() ||
        !request.contains("document") || !request["document"].is_object() ||
        !request.contains("state") || !request["state"].is_object()) {
        return Result<ProgressionPreviewSimulation>::Failure(Error(
            "PXWASM-PROGRESSION-001",
            "Progression simulation requires a canonical document and profile state."));
    }
    const Json& document = request["document"];
    const Json& state = request["state"];
    const std::string documentId =
        document.value("id", std::string{});
    if (document.value("format", std::string{}) != "PrismatiXProgression" ||
        document.value("schemaRevision", 0) != 2 || documentId.empty() ||
        !document.contains("revision") ||
        !document["revision"].is_number_unsigned() ||
        !document.contains("nodes") || !document["nodes"].is_array() ||
        !document.contains("edges") || !document["edges"].is_array() ||
        !document.contains("variables") || !document["variables"].is_array() ||
        !document.contains("carryOverPolicy") ||
        !document["carryOverPolicy"].is_object()) {
        return Result<ProgressionPreviewSimulation>::Failure(Error(
            "PXWASM-PROGRESSION-002",
            "Progression simulation rejected an incompatible canonical document schema.",
            documentId));
    }
    if (!state.contains("clearedRouteIds") ||
        !state["clearedRouteIds"].is_array() ||
        !state.contains("flags") || !state["flags"].is_array() ||
        !state.contains("clearCount") || !state["clearCount"].is_number_integer() ||
        state["clearCount"].get<std::int64_t>() < 0 ||
        !state.contains("variables") || !state["variables"].is_object()) {
        return Result<ProgressionPreviewSimulation>::Failure(Error(
            "PXWASM-PROGRESSION-STATE-001",
            "Progression profile state requires route ids, flags, a non-negative clear count, and literal variables.",
            documentId));
    }

    std::unordered_set<std::string> identities;
    std::unordered_set<std::string> nodeIds;
    std::unordered_set<std::string> startIds;
    for (const auto& node : document["nodes"]) {
        if (!node.is_object() || !node.contains("id") ||
            !node["id"].is_string() || !node.contains("kind") ||
            !node["kind"].is_string() || !node.contains("name") ||
            !node["name"].is_string() || !node.contains("x") ||
            !node["x"].is_number() || !node.contains("y") ||
            !node["y"].is_number()) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-NODE-001",
                "Progression nodes require stable identity, kind, name, and finite position.",
                documentId));
        }
        const auto id = node["id"].get<std::string>();
        const auto kind = node["kind"].get<std::string>();
        const auto name = node["name"].get<std::string>();
        const double x = node["x"].get<double>();
        const double y = node["y"].get<double>();
        if (id.empty() || name.empty() || IsBlank(name) ||
            !std::isfinite(x) || !std::isfinite(y) ||
            !identities.insert(id).second || !nodeIds.insert(id).second) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-NODE-002",
                "Progression node identities and names must be unique and positions finite.",
                documentId, id, "nodes"));
        }
        static const std::unordered_set<std::string> nodeKinds{
            "start",         "routeUnlock", "routeLock",
            "chapterUnlock", "sceneUnlock", "ending",
            "cgUnlock",      "galleryUnlock", "achievementFlag",
            "newGamePlus",   "trueRoute", "clearData",
        };
        if (!nodeKinds.contains(kind)) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-NODE-003",
                "Progression node kind is not part of the canonical schema.",
                documentId, id, "kind"));
        }
        const bool targetRequired =
            kind != "start" && kind != "newGamePlus" && kind != "clearData";
        const auto target = node.find("targetId");
        const bool targetPresent =
            target != node.end() && target->is_string() &&
            !target->get_ref<const std::string&>().empty() &&
            !IsBlank(target->get_ref<const std::string&>());
        const bool targetAbsent =
            target == node.end() || target->is_null() ||
            (target->is_string() && target->get_ref<const std::string&>().empty());
        if ((targetRequired && !targetPresent) ||
            (!targetRequired && !targetAbsent)) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-NODE-004",
                targetRequired
                    ? "This Progression node kind requires a stable target identity."
                    : "This Progression node kind cannot carry a target identity.",
                documentId, id, "targetId"));
        }
        if ((kind == "ending" || kind == "galleryUnlock" ||
             kind == "achievementFlag") &&
            !IsIdentifier(target->get_ref<const std::string&>())) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-NODE-005",
                "Semantic Progression targets must be stable ASCII identifiers.",
                documentId, id, "targetId"));
        }
        if (kind == "start") startIds.insert(id);
    }
    if (startIds.size() != 1) {
        return Result<ProgressionPreviewSimulation>::Failure(Error(
            "PXWASM-PROGRESSION-START-001",
            "Progression requires exactly one Start node.", documentId));
    }

    std::unordered_set<std::string> edgePairs;
    std::unordered_map<std::string, std::size_t> indegree;
    std::unordered_map<std::string, std::vector<std::string>> outgoing;
    std::unordered_map<std::string, std::vector<const Json*>> incoming;
    for (const auto& id : nodeIds) indegree.emplace(id, 0);
    for (const auto& edge : document["edges"]) {
        if (!edge.is_object() || !edge.contains("id") ||
            !edge["id"].is_string() || !edge.contains("from") ||
            !edge["from"].is_string() || !edge.contains("to") ||
            !edge["to"].is_string() || !edge.contains("requirements") ||
            !edge["requirements"].is_array()) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-EDGE-001",
                "Progression edges require stable identity, valid endpoints, and requirements.",
                documentId));
        }
        const auto id = edge["id"].get<std::string>();
        const auto from = edge["from"].get<std::string>();
        const auto to = edge["to"].get<std::string>();
        const auto pair = from + "\n" + to;
        if (id.empty() || from == to || !nodeIds.contains(from) ||
            !nodeIds.contains(to) || !identities.insert(id).second ||
            !edgePairs.insert(pair).second) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-EDGE-002",
                "Progression edges must have unique identities and distinct existing endpoints.",
                documentId, id, "edges"));
        }
        for (const auto& requirement : edge["requirements"]) {
            if (const auto invalid = ValidateRequirement(requirement, documentId))
                return Result<ProgressionPreviewSimulation>::Failure(*invalid);
            const auto requirementId = requirement["id"].get<std::string>();
            if (!identities.insert(requirementId).second) {
                return Result<ProgressionPreviewSimulation>::Failure(Error(
                    "PXWASM-PROGRESSION-REQUIREMENT-004",
                    "Progression requirement identities must be unique.",
                    documentId, requirementId, "requirements"));
            }
        }
        ++indegree[to];
        outgoing[from].push_back(to);
        incoming[to].push_back(&edge);
    }

    std::deque<std::string> ready;
    for (const auto& [id, degree] : indegree)
        if (degree == 0) ready.push_back(id);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto id = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto& target : outgoing[id])
            if (--indegree[target] == 0) ready.push_back(target);
    }
    if (visited != nodeIds.size()) {
        return Result<ProgressionPreviewSimulation>::Failure(Error(
            "PXWASM-PROGRESSION-CYCLE-001",
            "Progression dependencies cannot contain a cycle.", documentId));
    }

    std::unordered_set<std::string> declaredVariables;
    for (const auto& variable : document["variables"]) {
        if (!variable.is_object() || !variable.contains("id") ||
            !variable["id"].is_string() || !variable.contains("scope") ||
            !variable["scope"].is_string() || !variable.contains("name") ||
            !variable["name"].is_string() ||
            !variable.contains("defaultValue") ||
            !IsLiteral(variable["defaultValue"])) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-VARIABLE-001",
                "Progression variables require identity, scope, name, and a literal default value.",
                documentId));
        }
        const auto id = variable["id"].get<std::string>();
        const auto scope = variable["scope"].get<std::string>();
        const auto name = variable["name"].get<std::string>();
        const auto key = scope + "." + name;
        if (id.empty() || !identities.insert(id).second ||
            !IsVariableScope(scope) || !IsIdentifier(name) ||
            !declaredVariables.insert(key).second) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-VARIABLE-002",
                "Progression variable identities and scoped names must be unique and valid.",
                documentId, id, "variables"));
        }
    }
    const Json& carryOver = document["carryOverPolicy"];
    if (!carryOver.contains("variables") ||
        !carryOver["variables"].is_array() ||
        !carryOver.contains("seenText") || !carryOver["seenText"].is_boolean() ||
        !carryOver.contains("seenChoices") ||
        !carryOver["seenChoices"].is_boolean() ||
        !carryOver.contains("unlockedScenes") ||
        !carryOver["unlockedScenes"].is_boolean() ||
        !carryOver.contains("unlockedGallery") ||
        !carryOver["unlockedGallery"].is_boolean() ||
        !carryOver.contains("clearCount") ||
        !carryOver["clearCount"].is_boolean()) {
        return Result<ProgressionPreviewSimulation>::Failure(Error(
            "PXWASM-PROGRESSION-CARRY-OVER-001",
            "Progression carry-over policy is incomplete or incompatible.",
            documentId, {}, "carryOverPolicy"));
    }
    for (const auto& key : carryOver["variables"]) {
        if (!key.is_string() ||
            !declaredVariables.contains(key.get<std::string>())) {
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-CARRY-OVER-002",
                "Carry-over variables must reference declared Progression variables.",
                documentId, {}, "carryOverPolicy.variables"));
        }
    }
    for (const auto& edge : document["edges"]) {
        for (const auto& requirement : edge["requirements"]) {
            if (requirement["kind"].get<std::string>() == "variable" &&
                !declaredVariables.contains(
                    requirement["key"].get<std::string>())) {
                return Result<ProgressionPreviewSimulation>::Failure(Error(
                    "PXWASM-PROGRESSION-VARIABLE-003",
                    "Variable requirements must reference a declared Progression variable.",
                    documentId,
                    requirement["id"].get<std::string>(), "requirements.key"));
            }
        }
    }

    for (const auto& value : state["clearedRouteIds"])
        if (!value.is_string())
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-STATE-002",
                "Cleared route identities must be strings.", documentId));
    for (const auto& value : state["flags"])
        if (!value.is_string() || !IsIdentifier(value.get<std::string>()))
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-STATE-003",
                "Progression flags must be stable ASCII identifiers.", documentId));
    for (const auto& [key, value] : state["variables"].items())
        if (!IsScopedVariable(key) || !declaredVariables.contains(key) ||
            !IsLiteral(value))
            return Result<ProgressionPreviewSimulation>::Failure(Error(
                "PXWASM-PROGRESSION-STATE-004",
                "Progression simulation variables must reference declared scoped variables and carry literal values.",
                documentId, {}, "state.variables"));

    std::unordered_set<std::string> clearedRoutes;
    std::unordered_set<std::string> flags;
    for (const auto& value : state["clearedRouteIds"])
        clearedRoutes.insert(value.get<std::string>());
    for (const auto& value : state["flags"])
        flags.insert(value.get<std::string>());

    std::unordered_set<std::string> unlocked = startIds;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& edge : document["edges"]) {
            const auto from = edge["from"].get<std::string>();
            const auto to = edge["to"].get<std::string>();
            if (!unlocked.contains(from) || unlocked.contains(to)) continue;
            const bool requirementsMet = std::ranges::all_of(
                edge["requirements"], [&](const Json& requirement) {
                    return RequirementMet(requirement, state, clearedRoutes,
                                          flags);
                });
            if (requirementsMet) changed = unlocked.insert(to).second;
        }
    }

    ProgressionPreviewSimulation result;
    result.revision = document["revision"].get<std::uint64_t>();
    result.nodes.reserve(document["nodes"].size());
    for (const auto& node : document["nodes"]) {
        ProgressionNodeEvaluation evaluation;
        evaluation.nodeId = node["id"].get<std::string>();
        evaluation.unlocked = unlocked.contains(evaluation.nodeId);
        if (!evaluation.unlocked) {
            for (const Json* edge : incoming[evaluation.nodeId]) {
                if (!unlocked.contains((*edge)["from"].get<std::string>()))
                    continue;
                for (const auto& requirement : (*edge)["requirements"])
                    if (!RequirementMet(requirement, state, clearedRoutes,
                                        flags))
                        evaluation.unmetRequirementIds.push_back(
                            requirement["id"].get<std::string>());
            }
        }
        result.nodes.push_back(std::move(evaluation));
    }
    return Result<ProgressionPreviewSimulation>::Success(std::move(result));
}

}  // namespace px::preview
