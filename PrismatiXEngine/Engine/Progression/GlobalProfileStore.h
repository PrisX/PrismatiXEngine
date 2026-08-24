#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/Progression/GlobalProfile.h"

#include <string>

namespace px::progress {

bool LoadGlobalProfile(GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key);
bool SaveGlobalProfile(const GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key);

}  // namespace px::progress
