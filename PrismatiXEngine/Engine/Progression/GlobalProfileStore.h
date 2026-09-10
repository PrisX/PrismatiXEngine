#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/Persist.h"

#include <span>
#include <string>

namespace px::progress {

bool LoadGlobalProfile(GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key);
[[nodiscard]] PersistLoadStatus LoadGlobalProfileWithFallback(
    GlobalProfile& profile, const std::string& path,
    const crypto::Key* primaryKey,
    std::span<const crypto::Key> fallbackKeys = {});
bool SaveGlobalProfile(const GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key);
bool SaveGlobalProfilePreservingExisting(
    const GlobalProfile& profile, const std::string& path,
    const crypto::Key* primaryKey,
    std::span<const crypto::Key> fallbackKeys = {});

}  // namespace px::progress
