#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Platform/Input.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace px::progress {

struct GameSettings {
    int masterVolume = 128;
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
    WheelBindingMap wheelBindings;

    bool Load(const std::string& path, const crypto::Key* key);
    [[nodiscard]] PersistLoadStatus LoadWithFallback(
        const std::string& path, const crypto::Key* primaryKey,
        std::span<const crypto::Key> fallbackKeys = {});
    bool Save(const std::string& path, const crypto::Key* key) const;
    bool SavePreservingExisting(
        const std::string& path, const crypto::Key* primaryKey,
        std::span<const crypto::Key> fallbackKeys = {}) const;
    void CaptureInputBindings(const Input& input);
    [[nodiscard]] bool ApplyInputBindings(Input& input) const;
    [[nodiscard]] PersistLoadStatus LoadWithInputBindings(
        const std::string& path, Input& input, const crypto::Key* primaryKey,
        std::span<const crypto::Key> fallbackKeys = {});
    [[nodiscard]] bool SaveWithInputBindings(
        const std::string& path, const Input& input,
        const crypto::Key* primaryKey,
        std::span<const crypto::Key> fallbackKeys = {}) const;
};

}
