#pragma once

#include "Engine/IO/Crypto.h"

#include <string>

namespace px::progress {

struct GameSettings {
    int bgmVolume = 96;
    int seVolume = 110;
    int voiceVolume = 128;

    int textSpeedMs = 28;
    int autoWaitMs = 1400;
    bool skipReadOnly = true;
    bool skipAfterChoices = false;

    bool fullscreen = false;
    int windowWidth = 1280;
    int windowHeight = 720;
    std::string language = "zh-TW";
    float textScale = 1.0f;
    bool highContrast = false;
    bool reducedMotion = false;
    bool selfVoicing = false;

    bool Load(const std::string& path, const crypto::Key* key);
    bool Save(const std::string& path, const crypto::Key* key) const;
};

}
