#pragma once

#include <string>

namespace EngineConfig {
// Window & Resolution
constexpr int kDefaultScreenWidth = 1280;
constexpr int kDefaultScreenHeight = 720;
const std::string kGameTitle = "PrismatiX Engine";

// Paths & Archives
const std::string kArchiveEngine = "Engine.pdx";
const std::string kArchiveData = "Data.pdx";

// Save System
const std::string kSaveDirectory = "Save";
const std::string kSaveFilePrefix = "save_";
const std::string kSaveFileExt = ".sav";
}  // namespace EngineConfig
