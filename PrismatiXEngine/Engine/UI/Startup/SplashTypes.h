#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"

#include <optional>
#include <string>
#include <vector>

namespace px::ui::startup {

struct SplashScreenEntry {
    ResourceRefValue scene;
    std::optional<ResourceRefValue> audio;
    float minimumDuration = 2.0f;
    bool skippable = true;
    float skipAllowedAfter = 0.5f;
    std::string enterAnimation = "enter";
    std::string exitAnimation = "exit";

    auto operator<=>(const SplashScreenEntry&) const = default;
};

[[nodiscard]] Status ValidateSplashEntry(const SplashScreenEntry& entry,
                                         std::size_t index,
                                         const std::string& source = {});
[[nodiscard]] Result<std::vector<SplashScreenEntry>> ParseSplashSequence(
    const Variant& value, const std::string& source = {});
[[nodiscard]] Variant WriteSplashSequence(const std::vector<SplashScreenEntry>& entries);

}  // namespace px::ui::startup
