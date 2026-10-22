#include "Engine/Support/Logger.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace Logger {

namespace {
std::mutex g_mutex;
std::filesystem::path g_logPath;
}

bool Initialize(const std::string& application,
                const std::filesystem::path& directory) {
    std::lock_guard lock(g_mutex);
    try {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) throw std::filesystem::filesystem_error(
            "Could not create log directory", directory, ec);
        g_logPath = directory / (application + ".log");

        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_level(spdlog::level::debug);
        console->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");

        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            g_logPath.string(), 5u * 1024u * 1024u, 5u, false);
        file->set_level(spdlog::level::trace);
        file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

        std::vector<spdlog::sink_ptr> sinks{ console, file };
        auto logger = std::make_shared<spdlog::logger>(application, sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(std::move(logger));
        spdlog::flush_every(std::chrono::seconds(2));
        PX_LOG_INFO("logger initialized application={} path={}", application,
                    g_logPath.generic_string());
        return true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Log initialization failed: %s\n", ex.what());
        return false;
    }
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    spdlog::shutdown();
    // spdlog::shutdown() leaves no default logger, and PX_LOG_* would then
    // dereference null: an object torn down after this point (a static, or
    // an owner that outlives main's Logger scope) must not crash by logging.
    // A logger without sinks drops those lines instead.
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(std::string{}));
}

void Flush() {
    if (auto logger = spdlog::default_logger()) logger->flush();
}

std::filesystem::path CurrentLogPath() {
    std::lock_guard lock(g_mutex);
    return g_logPath;
}

}  // namespace Logger
