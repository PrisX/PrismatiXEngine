#pragma once

#include "Engine/VN/Scenario/ScenarioDocument.h"

#include <optional>

namespace px::vn::scenario {

struct StoryTarget {
    resource::ResourceId scenario;
    EntryId entry;
    std::string lastKnownPath;
};

struct StoryLink {
    resource::ResourceId sourceScenario;
    StatementId sourceStatement;
    std::string sourcePort;
    StoryTarget target;
};

[[nodiscard]] std::vector<StoryLink> DeriveStoryLinks(const ScenarioDocument& document);
Status ConnectStoryTarget(ScenarioDocument& source, const StatementId& statement,
                          std::string port, const StoryTarget& target);
Status DisconnectStoryTarget(ScenarioDocument& source, const StatementId& statement,
                             std::string_view port);
[[nodiscard]] std::optional<StoryTarget> GetStoryTarget(const ScenarioNode& node,
                                                       std::string_view port = "target");

}  // namespace px::vn::scenario
