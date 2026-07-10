#pragma once

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>

namespace Logger {

void Initialize(const std::string& application = "PrismatiX");
void Shutdown();
void Flush();
[[nodiscard]] std::filesystem::path CurrentLogPath();

}  // namespace Logger

#define PX_LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#define PX_LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define PX_LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define PX_LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define PX_LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define PX_LOG_CRITICAL(...) spdlog::critical(__VA_ARGS__)
