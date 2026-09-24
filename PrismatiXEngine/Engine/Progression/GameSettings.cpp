#include "Engine/Progression/GameSettings.h"

#include "Engine/Progression/Persist.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Utf8Path.h"

#include <algorithm>
#include <filesystem>

namespace px::progress {

bool GameSettings::Load(const std::string& path, const crypto::Key* key) {
    return LoadWithFallback(path, key) == PersistLoadStatus::Loaded;
}

PersistLoadStatus GameSettings::LoadWithFallback(
    const std::string& path, const crypto::Key* primaryKey,
    const std::span<const crypto::Key> fallbackKeys) {
    auto loaded = LoadJsonWithFallback(path, primaryKey, fallbackKeys);
    if (loaded.status != PersistLoadStatus::Loaded || !loaded.value)
        return loaded.status;
    const Json& j = *loaded.value;
    if (j.value("format", std::string{}) != "PrismatiXSettings" ||
        j.value("schemaRevision", 0) != 2) {
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSETTINGS6301",
            .category = "Persistence.Settings",
            .message = "Settings exist but their schema is invalid: " + path,
            .details = "The original settings were left unchanged."};
        diagnostic.source.path = path;
        diag::Emit(diagnostic);
        return PersistLoadStatus::Failed;
    }
    try {
        GameSettings candidate = *this;
        candidate.bgmVolume = j.value("bgmVolume", candidate.bgmVolume);
        candidate.seVolume = j.value("seVolume", candidate.seVolume);
        candidate.voiceVolume = j.value("voiceVolume", candidate.voiceVolume);
        candidate.textSpeedMs = j.value("textSpeedMs", candidate.textSpeedMs);
        candidate.autoWaitMs = j.value("autoWaitMs", candidate.autoWaitMs);
        candidate.skipReadOnly = j.value("skipReadOnly", candidate.skipReadOnly);
        candidate.fullscreen = j.value("fullscreen", candidate.fullscreen);
        candidate.windowWidth = j.value("windowWidth", candidate.windowWidth);
        candidate.windowHeight = j.value("windowHeight", candidate.windowHeight);
        candidate.language = j.value("language", candidate.language);
        candidate.textScale = std::clamp(
            j.value("textScale", candidate.textScale), 0.75f, 2.0f);
        candidate.highContrast =
            j.value("highContrast", candidate.highContrast);
        candidate.reducedMotion =
            j.value("reducedMotion", candidate.reducedMotion);
        candidate.selfVoicing =
            j.value("selfVoicing", candidate.selfVoicing);
        candidate.keyboardBindings.clear();
        if (j.contains("keyboardBindings")) {
            const auto& rawBindings = j.at("keyboardBindings");
            if (!rawBindings.is_object() || rawBindings.size() > 1'024)
                throw Json::type_error::create(
                    302, "keyboardBindings must be a bounded object", &rawBindings);
            for (auto it = rawBindings.begin(); it != rawBindings.end(); ++it) {
                if (!it.value().is_array() || it.value().size() > 32)
                    throw Json::type_error::create(
                        302, "each keyboard binding must be a bounded array", &it.value());
                candidate.keyboardBindings[it.key()] =
                    it.value().get<std::vector<int>>();
            }
        }
        *this = std::move(candidate);
        return PersistLoadStatus::Loaded;
    } catch (const Json::exception& error) {
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSETTINGS6302",
            .category = "Persistence.Settings",
            .message = "Settings payload contains invalid field types: " + path,
            .details = error.what()};
        diagnostic.source.path = path;
        diag::Emit(diagnostic);
        return PersistLoadStatus::Failed;
    }
}

bool GameSettings::Save(const std::string& path, const crypto::Key* key) const {
    Json j;
    j["format"] = "PrismatiXSettings";
    j["schemaRevision"] = 2;
    j["bgmVolume"] = bgmVolume;
    j["seVolume"] = seVolume;
    j["voiceVolume"] = voiceVolume;
    j["textSpeedMs"] = textSpeedMs;
    j["autoWaitMs"] = autoWaitMs;
    j["skipReadOnly"] = skipReadOnly;
    j["fullscreen"] = fullscreen;
    j["windowWidth"] = windowWidth;
    j["windowHeight"] = windowHeight;
    j["language"] = language;
    j["textScale"] = textScale;
    j["highContrast"] = highContrast;
    j["reducedMotion"] = reducedMotion;
    j["selfVoicing"] = selfVoicing;
    if (!keyboardBindings.empty()) j["keyboardBindings"] = keyboardBindings;
    return SaveJson(path, j, key);
}

bool GameSettings::SavePreservingExisting(
    const std::string& path, const crypto::Key* primaryKey,
    const std::span<const crypto::Key> fallbackKeys) const {
    if (std::filesystem::exists(PathFromUtf8(path))) {
        const auto loaded =
            LoadJsonWithFallback(path, primaryKey, fallbackKeys);
        if (!loaded.Loaded()) return false;
        GameSettings existing;
        if (existing.LoadWithFallback(path, primaryKey, fallbackKeys) !=
            PersistLoadStatus::Loaded) {
            return false;
        }
        if (loaded.UsedFallback() && !BackupPersistenceFile(path)) return false;
    }
    return Save(path, primaryKey);
}

}
