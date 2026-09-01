#include "Engine/Progression/GameSettings.h"

#include "Engine/Progression/Persist.h"

#include <algorithm>

namespace px::progress {

bool GameSettings::Load(const std::string& path, const crypto::Key* key) {
    auto json = LoadJson(path, key);
    if (!json) {
        return false;
    }
    const Json& j = *json;
    if (j.value("format", std::string{}) != "PrismatiXSettings" ||
        j.value("schemaRevision", 0) != 2) {
        return false;
    }
    bgmVolume = j.value("bgmVolume", bgmVolume);
    seVolume = j.value("seVolume", seVolume);
    voiceVolume = j.value("voiceVolume", voiceVolume);
    textSpeedMs = j.value("textSpeedMs", textSpeedMs);
    autoWaitMs = j.value("autoWaitMs", autoWaitMs);
    skipReadOnly = j.value("skipReadOnly", skipReadOnly);
    skipAfterChoices = j.value("skipAfterChoices", skipAfterChoices);
    fullscreen = j.value("fullscreen", fullscreen);
    windowWidth = j.value("windowWidth", windowWidth);
    windowHeight = j.value("windowHeight", windowHeight);
    language = j.value("language", language);
    textScale = std::clamp(j.value("textScale", textScale), 0.75f, 2.0f);
    highContrast = j.value("highContrast", highContrast);
    reducedMotion = j.value("reducedMotion", reducedMotion);
    selfVoicing = j.value("selfVoicing", selfVoicing);
    return true;
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
    j["skipAfterChoices"] = skipAfterChoices;
    j["fullscreen"] = fullscreen;
    j["windowWidth"] = windowWidth;
    j["windowHeight"] = windowHeight;
    j["language"] = language;
    j["textScale"] = textScale;
    j["highContrast"] = highContrast;
    j["reducedMotion"] = reducedMotion;
    j["selfVoicing"] = selfVoicing;
    return SaveJson(path, j, key);
}

}
