#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "Engine/Core/Result.h"
#include "Engine/VN/Runtime/Program.h"

namespace px {

using RuntimeAssetExists = std::function<bool(std::string_view path)>;

// Runtime IR uses `asset:<uuid>` tokens so authoring-time paths never become
// authoritative. This boundary resolves every token against project.pxproject
// before a Program reaches the VM. The returned Program is an all-or-nothing
// copy: an invalid token, unknown UUID, or missing file leaves the caller's
// currently running program untouched.
[[nodiscard]] bool UsesRuntimeAssetReferences(const vn::Program& program);

[[nodiscard]] Result<vn::Program> ResolveRuntimeAssetReferences(vn::Program program, std::string_view projectManifest, const RuntimeAssetExists& exists, const std::string& sourcePath);

}  // namespace px
