#pragma once

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <vector>

namespace Logger {
inline void Initialize() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::trace);
        console_sink->set_pattern("[%H:%M:%S] [%^%l%$] %v");

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/engine.log", true);
        file_sink->set_level(spdlog::level::trace);

        std::vector<spdlog::sink_ptr> sinks{ console_sink, file_sink };
        auto logger = std::make_shared<spdlog::logger>("PrismatiX", sinks.begin(), sinks.end());

        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(logger);

        spdlog::info("--- Logger Initialized ---");
    } catch (const spdlog::spdlog_ex& ex) {
        printf("Log initialization failed: %s\n", ex.what());
    }
}
}

#define PX_LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#define PX_LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define PX_LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define PX_LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define PX_LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define PX_LOG_CRITICAL(...) spdlog::critical(__VA_ARGS__)
