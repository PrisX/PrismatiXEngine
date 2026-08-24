#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace px::preview {

struct ProgressionNodeEvaluation {
    std::string nodeId;
    bool unlocked = false;
    std::vector<std::string> unmetRequirementIds;
};

struct ProgressionPreviewSimulation {
    std::uint64_t revision = 0;
    std::vector<ProgressionNodeEvaluation> nodes;
};

// Validates and evaluates the canonical Progression document inside
// RuntimeCore. Both the native and Emscripten compositions call this contract;
// Authoring frontends must not reproduce these semantics in JavaScript.
[[nodiscard]] Result<ProgressionPreviewSimulation>
SimulateProgressionPreview(std::string_view requestJson);

}  // namespace px::preview
