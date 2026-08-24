#include "Engine/Progression/GlobalProfileStore.h"

#include "Engine/Progression/Persist.h"

namespace px::progress {

bool LoadGlobalProfile(GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key) {
    const auto json = LoadJson(path, key);
    return json && profile.ApplyJson(*json);
}

bool SaveGlobalProfile(const GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key) {
    return SaveJson(path, profile.ToJson(), key);
}

}  // namespace px::progress
