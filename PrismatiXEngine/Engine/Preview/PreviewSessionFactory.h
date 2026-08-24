#pragma once

#include "Engine/Core/Result.h"
#include "Engine/SDK/PreviewSession.h"

#include <functional>
#include <memory>
#include <optional>

namespace px {
class RuntimeSession;
namespace vn {
struct Command;
}
}  // namespace px

namespace px::preview {

struct PreviewSessionOptions {
    bool previewSafeMode = true;
    std::size_t checkpointInterval = 100;
    std::size_t maximumCheckpoints = 128;
    std::function<std::optional<sdk::PreviewSafety>(const vn::Command&)>
        inspectSafety;
    std::function<Status(int width, int height, float scale)> resize;
    std::function<std::shared_ptr<const void>()> captureExternalState;
    std::function<Status(const std::shared_ptr<const void>&)>
        restoreExternalState;
};

[[nodiscard]] std::unique_ptr<sdk::PreviewSession> CreatePreviewSession(
    RuntimeSession& runtime, PreviewSessionOptions options = {});

}  // namespace px::preview
