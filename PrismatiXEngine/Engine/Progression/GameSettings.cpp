#include "Engine/Progression/GameSettings.h"

#include "Engine/Progression/Persist.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Utf8Path.h"

#include <algorithm>
#include <cmath>
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
    if (!j.is_object() || !j.contains("format") ||
        !j.at("format").is_string() ||
        j.at("format") != "PrismatiXSettings" ||
        !j.contains("schemaRevision") ||
        !j.at("schemaRevision").is_number_integer() ||
        j.at("schemaRevision") != 1) {
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
        candidate.masterVolume = j.value("masterVolume", candidate.masterVolume);
        candidate.bgmVolume = j.value("bgmVolume", candidate.bgmVolume);
        candidate.seVolume = j.value("seVolume", candidate.seVolume);
        candidate.voiceVolume = j.value("voiceVolume", candidate.voiceVolume);
        candidate.masterMuted = j.value("masterMuted", candidate.masterMuted);
        candidate.bgmMuted = j.value("bgmMuted", candidate.bgmMuted);
        candidate.seMuted = j.value("seMuted", candidate.seMuted);
        candidate.voiceMuted = j.value("voiceMuted", candidate.voiceMuted);
        candidate.duckBgmOnVoice = j.value("duckBgmOnVoice", candidate.duckBgmOnVoice);
        candidate.continueVoiceOnAdvance = j.value("continueVoiceOnAdvance", candidate.continueVoiceOnAdvance);
        candidate.voiceProfileVolumes.clear();
        if (j.contains("voiceProfileVolumes")) {
            const auto& profiles = j.at("voiceProfileVolumes");
            if (!profiles.is_object() || profiles.size() > 128)
                throw Json::type_error::create(302, "voice profiles must be a bounded object", &profiles);
            for (auto it = profiles.begin(); it != profiles.end(); ++it) {
                if (it.key().empty() || it.key().size() > 64 || !it.value().is_number_integer())
                    throw Json::type_error::create(302, "voice profile is invalid", &it.value());
                candidate.voiceProfileVolumes[it.key()] = std::clamp(it.value().get<int>(), 0, 128);
            }
        }
        candidate.voiceProfileMuted.clear();
        if (j.contains("voiceProfileMuted")) {
            const auto& muted = j.at("voiceProfileMuted");
            if (!muted.is_object() || muted.size() > 128)
                throw Json::type_error::create(302, "voice mutes must be a bounded object", &muted);
            for (auto it = muted.begin(); it != muted.end(); ++it) {
                if (it.key().empty() || it.key().size() > 64 || !it.value().is_boolean())
                    throw Json::type_error::create(302, "voice mute is invalid", &it.value());
                candidate.voiceProfileMuted[it.key()] = it.value().get<bool>();
            }
        }
        candidate.options.clear();
        if (j.contains("options")) {
            const auto& stored = j.at("options");
            if (!stored.is_object() || stored.size() > 256)
                throw Json::type_error::create(302, "options must be a bounded object", &stored);
            for (auto it = stored.begin(); it != stored.end(); ++it) {
                if (it.key().empty() || it.key().size() > 64 || !it.value().is_number_integer())
                    throw Json::type_error::create(302, "option is invalid", &it.value());
                candidate.options[it.key()] = std::clamp(it.value().get<int>(), -1'000'000, 1'000'000);
            }
        }
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
        candidate.wheelBindings.clear();
        if (j.contains("wheelBindings")) {
            const auto& raw = j.at("wheelBindings");
            if (!raw.is_object() || raw.size() > 1'024)
                throw Json::type_error::create(
                    302, "wheelBindings must be a bounded object", &raw);
            for (auto it = raw.begin(); it != raw.end(); ++it) {
                const auto& value = it.value();
                if (it.key().empty() || it.key().size() > 512 ||
                    it.key().find('\0') != std::string::npos ||
                    !value.is_object() || value.size() != 2 ||
                    !value.contains("direction") ||
                    !value.at("direction").is_string() ||
                    !value.contains("threshold") ||
                    !value.at("threshold").is_number())
                    throw Json::type_error::create(
                        302, "wheel binding is invalid", &value);
                const std::string direction =
                    value.at("direction").get<std::string>();
                const float threshold = value.at("threshold").get<float>();
                if (!std::isfinite(threshold) || threshold < 0.0f ||
                    threshold > 100.0f ||
                    (direction != "none" && direction != "up" &&
                     direction != "down") ||
                    (direction == "none" && threshold != 0.0f))
                    throw Json::type_error::create(
                        302, "wheel binding direction or threshold is invalid",
                        &value);
                candidate.wheelBindings[it.key()] = {
                    direction == "up" ? WheelDirection::Up
                        : direction == "down" ? WheelDirection::Down
                                               : WheelDirection::None,
                    threshold};
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
    j["schemaRevision"] = 1;
    j["masterVolume"] = masterVolume;
    j["bgmVolume"] = bgmVolume;
    j["seVolume"] = seVolume;
    j["voiceVolume"] = voiceVolume;
    j["masterMuted"] = masterMuted;
    j["bgmMuted"] = bgmMuted;
    j["seMuted"] = seMuted;
    j["voiceMuted"] = voiceMuted;
    j["duckBgmOnVoice"] = duckBgmOnVoice;
    j["continueVoiceOnAdvance"] = continueVoiceOnAdvance;
    if (!voiceProfileVolumes.empty()) j["voiceProfileVolumes"] = voiceProfileVolumes;
    if (!voiceProfileMuted.empty()) j["voiceProfileMuted"] = voiceProfileMuted;
    if (!options.empty()) j["options"] = options;
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
    if (!wheelBindings.empty()) {
        if (wheelBindings.size() > 1'024) return false;
        Json wheel = Json::object();
        for (const auto& [action, binding] : wheelBindings) {
            if (action.empty() || action.size() > 512 ||
                action.find('\0') != std::string::npos ||
                !std::isfinite(binding.threshold) ||
                binding.threshold < 0.0f || binding.threshold > 100.0f ||
                (binding.direction == WheelDirection::None &&
                 binding.threshold != 0.0f)) return false;
            const char* direction = binding.direction == WheelDirection::Up
                ? "up" : binding.direction == WheelDirection::Down
                ? "down" : binding.direction == WheelDirection::None
                ? "none" : nullptr;
            if (!direction) return false;
            wheel[action] = {{"direction", direction},
                             {"threshold", binding.threshold}};
        }
        j["wheelBindings"] = std::move(wheel);
    }
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

void GameSettings::CaptureInputBindings(const Input& input) {
    keyboardBindings = input.CaptureKeyboardBindings();
    wheelBindings = input.CaptureWheelBindings();
}

bool GameSettings::ApplyInputBindings(Input& input) const {
    return input.RestoreUserBindings(keyboardBindings, wheelBindings);
}

PersistLoadStatus GameSettings::LoadWithInputBindings(
    const std::string& path, Input& input, const crypto::Key* primaryKey,
    const std::span<const crypto::Key> fallbackKeys) {
    GameSettings candidate = *this;
    const auto status = candidate.LoadWithFallback(path, primaryKey,
                                                    fallbackKeys);
    if (status != PersistLoadStatus::Loaded) return status;
    if (!candidate.ApplyInputBindings(input)) {
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSETTINGS6303",
            .category = "Persistence.Settings",
            .message = "Settings contain invalid input bindings: " + path,
            .details = "The original settings and input bindings were left unchanged."};
        diagnostic.source.path = path;
        diag::Emit(diagnostic);
        return PersistLoadStatus::Failed;
    }
    *this = std::move(candidate);
    return PersistLoadStatus::Loaded;
}

bool GameSettings::SaveWithInputBindings(
    const std::string& path, const Input& input,
    const crypto::Key* primaryKey,
    const std::span<const crypto::Key> fallbackKeys) const {
    GameSettings candidate = *this;
    candidate.CaptureInputBindings(input);
    return candidate.SavePreservingExisting(path, primaryKey, fallbackKeys);
}

}
