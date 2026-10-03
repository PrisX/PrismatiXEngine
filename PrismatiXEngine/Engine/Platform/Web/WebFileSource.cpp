#if defined(PRISMATIX_WEB_PLAYER)
#include "Engine/IO/FileSource.h"

#include <emscripten/emscripten.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace px::io {
namespace {
EM_JS(double, WebFileSize, (const char* path), {
    return Module.prismatix.files.size(UTF8ToString(path));
});
EM_ASYNC_JS(int, WebRead, (const char* path, double offset, int count, void* output), {
    try {
        const bytes = await Module.prismatix.files.read(UTF8ToString(path), offset, count);
        HEAPU8.set(bytes, output);
        return bytes.length;
    } catch (error) {
        Module.prismatix.error(error);
        return -1;
    }
});
EM_JS(char*, WebList, (const char* path), {
    const files = Module.prismatix.files.list(UTF8ToString(path));
    return files === null ? 0 : stringToNewUTF8(JSON.stringify(files));
});

class WebReadStream final : public SeekableReadStream {
public:
    WebReadStream(std::string path, const std::uint64_t size)
        : m_path(std::move(path)), m_size(size) {}
    std::size_t Read(std::uint8_t* destination, const std::size_t bytes) override {
        if (m_failed || !destination || !bytes || m_position == m_size) return 0;
        const auto count = static_cast<int>(std::min<std::uint64_t>(
            {bytes, m_size - m_position, 1024u * 1024u}));
        const int read = WebRead(m_path.c_str(), static_cast<double>(m_position),
                                 count, destination);
        if (read <= 0 || read > count) { m_failed = true; return 0; }
        m_position += static_cast<std::uint64_t>(read);
        return static_cast<std::size_t>(read);
    }
    bool Seek(const std::int64_t offset, const SeekOrigin origin) override {
        const auto base = origin == SeekOrigin::Begin ? 0u
                        : origin == SeekOrigin::End ? m_size : m_position;
        if (m_failed || base > static_cast<std::uint64_t>(INT64_MAX)) return false;
        if (offset > 0 && base > static_cast<std::uint64_t>(INT64_MAX - offset))
            return false;
        if (offset < 0 && static_cast<std::uint64_t>(-(offset + 1)) + 1 > base)
            return false;
        const auto next = static_cast<std::int64_t>(base) + offset;
        if (next < 0 || static_cast<std::uint64_t>(next) > m_size) return false;
        m_position = static_cast<std::uint64_t>(next);
        return true;
    }
    std::uint64_t Tell() const override { return m_position; }
    std::uint64_t Size() const override { return m_size; }
    bool Failed() const override { return m_failed; }
private:
    std::string m_path;
    std::uint64_t m_size = 0, m_position = 0;
    bool m_failed = false;
};

class WebSource final : public FileSource {
public:
    std::unique_ptr<SeekableReadStream> Open(const std::string_view path) const override {
        const auto size = FileSize(path);
        return size ? std::make_unique<WebReadStream>(std::string(path), *size) : nullptr;
    }
    std::optional<std::uint64_t> FileSize(const std::string_view path) const override {
        const auto size = WebFileSize(std::string(path).c_str());
        if (!std::isfinite(size) || size < 0 || size > 9007199254740991.0)
            return std::nullopt;
        return static_cast<std::uint64_t>(size);
    }
    std::optional<std::vector<std::string>> ListFiles(const std::string_view path) const override;
    std::optional<std::string> CanonicalRoot(const std::string_view root) const override {
        auto path = std::filesystem::path(root).lexically_normal();
        if (!path.is_absolute()) path = std::filesystem::path("/game") / path;
        return path.lexically_normal().generic_string();
    }
    std::optional<std::string> ResolveUnder(const std::string_view root,
                                           const std::string_view relative) const override {
        const auto base = std::filesystem::path(root).lexically_normal();
        const auto path = (base / relative).lexically_normal();
        const auto within = path.lexically_relative(base);
        if (within.empty() || within.is_absolute() || *within.begin() == "..")
            return std::nullopt;
        return path.generic_string();
    }
    std::string_view Name() const override { return "web"; }
};
} // namespace
} // namespace px::io

#include <nlohmann/json.hpp>
namespace px::io {
std::optional<std::vector<std::string>> WebSource::ListFiles(const std::string_view path) const {
    char* json = WebList(std::string(path).c_str());
    if (!json) return std::nullopt;
    const std::string text(json);
    std::free(json);
    try { return nlohmann::json::parse(text).get<std::vector<std::string>>(); }
    catch (...) { return std::nullopt; }
}
std::shared_ptr<const FileSource> WebFileSource() {
    static const auto source = std::make_shared<WebSource>();
    return source;
}
} // namespace px::io
#endif
