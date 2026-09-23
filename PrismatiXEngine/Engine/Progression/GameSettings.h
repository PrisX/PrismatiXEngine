#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/Progression/Persist.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace px::progress {

struct GameSettings {
    int bgmVolume = 96;
    int seVolume = 110;
    int voiceVolume = 128;

    int textSpeedMs = 28;
    int autoWaitMs = 1400;
    bool skipReadOnly = true;

    bool fullscreen = false;
    int windowWidth = 1280;
    int windowHeight = 720;
    std::string language = "zh-TW";
    float textScale = 1.0f;
    bool highContrast = false;
    bool reducedMotion = false;
    bool selfVoicing = false;
    // Optional keyboard overrides. Empty means engine defaults; unknown
    // action names remain available for scripts that register them later.
    std::map<std::string, std::vector<int>, std::less<>> keyboardBindings;

    bool Load(const std::string& path, const crypto::Key* key);
    [[nodiscard]] PersistLoadStatus LoadWithFallback(
        const std::string& path, const crypto::Key* primaryKey,
        std::span<const crypto::Key> fallbackKeys = {});
    bool Save(const std::string& path, const crypto::Key* key) const;
    bool SavePreservingExisting(
        const std::string& path, const crypto::Key* primaryKey,
        std::span<const crypto::Key> fallbackKeys = {}) const;
};

}
