#include "Engine/Progression/GlobalProfileStore.h"

#include "Engine/Progression/Persist.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace px::progress {

bool LoadGlobalProfile(GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key) {
    return LoadGlobalProfileWithFallback(profile, path, key) ==
           PersistLoadStatus::Loaded;
}

PersistLoadStatus LoadGlobalProfileWithFallback(
    GlobalProfile& profile, const std::string& path,
    const crypto::Key* primaryKey,
    const std::span<const crypto::Key> fallbackKeys) {
    auto loaded = LoadJsonWithFallback(path, primaryKey, fallbackKeys);
    if (loaded.status != PersistLoadStatus::Loaded || !loaded.value)
        return loaded.status;
    GlobalProfile candidate = profile;
    if (!candidate.ApplyJson(*loaded.value)) {
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXPROFILE6201",
            .category = "Persistence.Profile",
            .message = "Profile exists but its schema or payload is invalid: " +
                       path,
            .details = "The original profile was left unchanged."};
        diagnostic.source.path = path;
        diag::Emit(diagnostic);
        return PersistLoadStatus::Failed;
    }
    profile = std::move(candidate);
    return PersistLoadStatus::Loaded;
}

bool SaveGlobalProfile(const GlobalProfile& profile, const std::string& path,
                       const crypto::Key* key) {
    nlohmann::json encoded;
    return profile.TryToJson(encoded) && SaveJson(path, encoded, key);
}

bool SaveGlobalProfilePreservingExisting(
    const GlobalProfile& profile, const std::string& path,
    const crypto::Key* primaryKey,
    const std::span<const crypto::Key> fallbackKeys) {
    if (std::filesystem::exists(PathFromUtf8(path))) {
        const auto loaded =
            LoadJsonWithFallback(path, primaryKey, fallbackKeys);
        if (!loaded.Loaded()) return false;
        GlobalProfile existing;
        if (!existing.ApplyJson(*loaded.value)) return false;
        if (loaded.UsedFallback() && !BackupPersistenceFile(path)) return false;
    }
    return SaveGlobalProfile(profile, path, primaryKey);
}

}  // namespace px::progress
