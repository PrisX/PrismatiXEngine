#pragma once

#include "Engine/SDK/RuntimeIr.h"
#include "Engine/VN/Runtime/Program.h"

namespace px {

// Converts public Runtime IR into the same VM Program consumed by Player.
// Preview-specific behavior belongs around RuntimeSession, not in this adapter.
[[nodiscard]] vn::Program CompileRuntimeIr(const sdk::RuntimeIrDocument& document);

}  // namespace px
