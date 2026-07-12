#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Resources/ResourceRef.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Runtime/Program.h"

#include <string>
#include <vector>

namespace px::vn::scenario {

using StatementId = Uuid;
using EntryId = Uuid;

struct ScenarioNode {
    StatementId id;
    std::string command;
    VariantObject parameters;
};

struct ScenarioEdge {
    Uuid id;
    StatementId fromNode;
    std::string fromPort = "flow";
    StatementId toNode;
    std::string toPort = "in";
};

struct ScenarioDocument {
    static constexpr int CurrentVersion = 4;
    int version = CurrentVersion;
    resource::ResourceId id;
    std::string name;
    EntryId entry;
    std::vector<ScenarioNode> nodes;
    std::vector<ScenarioEdge> edges;
};

struct NodeLayout {
    StatementId node;
    Vec2 position;
    Vec2 size;
    std::string group;
};

struct ScenarioLayoutDocument {
    static constexpr int CurrentVersion = 4;
    int version = CurrentVersion;
    resource::ResourceId scenario;
    std::vector<NodeLayout> nodes;
};

struct ValidationReport {
    std::vector<diag::Diagnostic> diagnostics;
    [[nodiscard]] bool Valid() const;
};

[[nodiscard]] Result<ScenarioDocument> ParseScenario(std::string_view text,
                                                     const std::string& sourcePath = {});
[[nodiscard]] std::string WriteScenario(const ScenarioDocument& document);
[[nodiscard]] Result<ScenarioLayoutDocument> ParseScenarioLayout(
    std::string_view text, const std::string& sourcePath = {});
[[nodiscard]] std::string WriteScenarioLayout(const ScenarioLayoutDocument& document);
[[nodiscard]] ValidationReport ValidateScenario(
    const ScenarioDocument& document,
    const CommandRegistry& registry = CommandRegistry::Builtins(),
    const std::string& sourcePath = {});

[[nodiscard]] Program CompileScenario(const ScenarioDocument& document,
                                      const CommandRegistry& registry = CommandRegistry::Builtins());

}  // namespace px::vn::scenario
