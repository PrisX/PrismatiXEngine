#pragma once

#include <string_view>

namespace EngineConfig {
// Window & Resolution
constexpr int kDefaultScreenWidth = 1280;
constexpr int kDefaultScreenHeight = 720;
constexpr std::string_view kGameTitle = "PrismatiX Engine";

// Paths & Archives
constexpr std::string_view kArchiveEngine = "Engine.pdx";
constexpr std::string_view kArchiveData = "Data.pdx";

// Save System
constexpr std::string_view kSaveDirectory = "Save";
constexpr std::string_view kSaveFilePrefix = "save_";
constexpr std::string_view kSaveFileExt = ".sav";

// Rendering
inline int kFontOversample = 2;
}  // namespace EngineConfig
