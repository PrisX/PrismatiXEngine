#include "Engine/IO/VFS.h"

#include "Engine/Support/Logger.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>

namespace px::io {

namespace {
class FileReadStream final : public SeekableReadStream {
public:
    explicit FileReadStream(const std::filesystem::path& path)
        : m_input(path, std::ios::binary) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (!m_input || error) {
            m_failed = true;
            return;
        }
        m_size = size;
    }

    [[nodiscard]] std::size_t Read(std::uint8_t* destination,
                                   const std::size_t bytes) override {
        if (m_failed || !destination || bytes == 0 || m_position >= m_size)
            return 0;
        const std::uint64_t remaining = m_size - m_position;
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining, std::min<std::uint64_t>(
                           bytes, static_cast<std::uint64_t>(
                                      (std::numeric_limits<std::streamsize>::max)()))));
        m_input.read(reinterpret_cast<char*>(destination),
                     static_cast<std::streamsize>(count));
        const std::streamsize read = m_input.gcount();
        if (read < 0 || read != static_cast<std::streamsize>(count)) {
            m_failed = true;
            if (read <= 0) return 0;
        }
        m_position += static_cast<std::uint64_t>(read);
        return static_cast<std::size_t>(read);
    }

    [[nodiscard]] bool Seek(const std::int64_t offset,
                            const SeekOrigin origin) override {
        if (m_failed) return false;
        std::int64_t base = 0;
        if (origin == SeekOrigin::Current) {
            if (m_position > static_cast<std::uint64_t>(
                                 (std::numeric_limits<std::int64_t>::max)()))
                return false;
            base = static_cast<std::int64_t>(m_position);
        } else if (origin == SeekOrigin::End) {
            if (m_size > static_cast<std::uint64_t>(
                             (std::numeric_limits<std::int64_t>::max)()))
                return false;
            base = static_cast<std::int64_t>(m_size);
        }
        if ((offset > 0 && base > (std::numeric_limits<std::int64_t>::max)() - offset) ||
            (offset < 0 && base < (std::numeric_limits<std::int64_t>::min)() - offset))
            return false;
        const std::int64_t requested = base + offset;
        if (requested < 0 || static_cast<std::uint64_t>(requested) > m_size)
            return false;
        m_input.clear();
        m_input.seekg(static_cast<std::streamoff>(requested), std::ios::beg);
        if (!m_input) {
            m_failed = true;
            return false;
        }
        m_position = static_cast<std::uint64_t>(requested);
        return true;
    }

    [[nodiscard]] std::uint64_t Tell() const override { return m_position; }
    [[nodiscard]] std::uint64_t Size() const override { return m_size; }
    [[nodiscard]] bool Failed() const override { return m_failed; }

private:
    std::ifstream m_input;
    std::uint64_t m_size = 0;
    std::uint64_t m_position = 0;
    bool m_failed = false;
};

bool IsValidUtf8(const std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (lead <= 0x7f) {
            ++index;
            continue;
        }
        if ((lead & 0xe0u) == 0xc0u) {
            continuation = 1;
            codepoint = lead & 0x1fu;
            if (codepoint == 0) return false;  // overlong ASCII
        } else if ((lead & 0xf0u) == 0xe0u) {
            continuation = 2;
            codepoint = lead & 0x0fu;
        } else if ((lead & 0xf8u) == 0xf0u) {
            continuation = 3;
            codepoint = lead & 0x07u;
        } else {
            return false;
        }
        if (index + continuation >= text.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if ((byte & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (byte & 0x3fu);
        }
        if ((continuation == 1 && codepoint < 0x80u) ||
            (continuation == 2 && codepoint < 0x800u) ||
            (continuation == 3 && codepoint < 0x10000u) ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint > 0x10ffffu) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

std::optional<std::filesystem::path> ResolveDirectoryPath(
    const std::string& root, const std::string_view normalized) {
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(std::filesystem::path(root), error);
    if (error) return std::nullopt;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(
        canonicalRoot / std::filesystem::path(normalized), error);
    if (error) return std::nullopt;
    const std::filesystem::path relative =
        std::filesystem::relative(candidate, canonicalRoot, error);
    if (error || relative.empty() || relative.is_absolute()) return std::nullopt;
    const auto first = relative.begin();
    if (first == relative.end() || *first == "..") return std::nullopt;
    return candidate;
}
}

std::optional<std::string> VFS::NormalizeVirtualPath(
    const std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\0') != path.npos ||
        path.find('\\') != path.npos || path.find(':') != path.npos ||
        !IsValidUtf8(path)) {
        return std::nullopt;
    }
    std::size_t segmentStart = 0;
    while (segmentStart <= path.size()) {
        const std::size_t slash = path.find('/', segmentStart);
        const std::size_t segmentEnd =
            slash == path.npos ? path.size() : slash;
        const std::string_view segment =
            path.substr(segmentStart, segmentEnd - segmentStart);
        if (segment.empty() || segment == "." || segment == "..") {
            return std::nullopt;
        }
        if (slash == path.npos) break;
        segmentStart = slash + 1;
    }
    return std::string(path);
}

void VFS::MountDirectory(const std::string& root) {
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(std::filesystem::path(root), error);
    if (error || canonical.empty()) {
        PX_LOG_ERROR("VFS rejected invalid directory mount '{}'", root);
        return;
    }
    m_dirs.push_back(canonical.string());
    PX_LOG_INFO("VFS mounted directory '{}'", canonical.string());
}

bool VFS::MountArchive(const std::string& archivePath, const crypto::Key* key) {
#if defined(PRISMATIX_PREVIEW_WASM)
    (void)archivePath;
    (void)key;
    PX_LOG_ERROR("Archive mounts are unavailable in the WASM preview VFS");
    return false;
#else
    auto archive = std::make_unique<Archive>();
    if (!archive->Open(archivePath, key)) {
        return false;
    }
    m_archives.push_back(std::move(archive));
    return true;
#endif
}

void VFS::Clear() {
    m_dirs.clear();
    m_archives.clear();
}

bool VFS::Exists(std::string_view path) const {
    const auto normalized = NormalizeVirtualPath(path);
    if (!normalized) return false;
    for (const std::string& dir : m_dirs) {
        std::error_code error;
        const auto full = ResolveDirectoryPath(dir, *normalized);
        if (full && std::filesystem::is_regular_file(*full, error) && !error) {
            return true;
        }
    }
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& archive : m_archives) {
        if (archive->Contains(*normalized)) {
            return true;
        }
    }
#endif
    return false;
}

std::unique_ptr<SeekableReadStream> VFS::Open(const std::string_view path) const {
    const auto normalized = NormalizeVirtualPath(path);
    if (!normalized) return {};
    for (const std::string& dir : m_dirs) {
        std::error_code error;
        const auto full = ResolveDirectoryPath(dir, *normalized);
        if (!full || !std::filesystem::is_regular_file(*full, error) || error)
            continue;
        auto stream = std::make_unique<FileReadStream>(*full);
        if (!stream->Failed()) return stream;
    }
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& archive : m_archives) {
        if (auto stream = archive->OpenStream(*normalized)) return stream;
    }
#endif
    return {};
}

std::optional<Bytes> VFS::Read(std::string_view path) const {
    const auto normalized = NormalizeVirtualPath(path);
    if (!normalized) return std::nullopt;
    for (const std::string& dir : m_dirs) {
        const auto resolved = ResolveDirectoryPath(dir, *normalized);
        if (!resolved) continue;
        const std::filesystem::path& full = *resolved;
        std::error_code error;
        if (!std::filesystem::is_regular_file(full, error) || error) continue;
        std::ifstream in(full, std::ios::binary | std::ios::ate);
        if (!in) continue;
        const std::streamoff length = static_cast<std::streamoff>(in.tellg());
        if (length < 0 ||
            static_cast<std::uintmax_t>(length) >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<std::size_t>::max()) ||
            static_cast<std::uintmax_t>(length) >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<std::streamsize>::max())) {
            continue;
        }
        in.seekg(0, std::ios::beg);
        if (!in) continue;
        Bytes data(static_cast<std::size_t>(length));
        if (data.empty()) return data;
        const auto size = static_cast<std::streamsize>(data.size());
        if (in.read(reinterpret_cast<char*>(data.data()), size) &&
            in.gcount() == size) {
            return data;
        }
    }
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& archive : m_archives) {
        if (auto data = archive->Read(*normalized)) {
            return data;
        }
    }
#endif
    return std::nullopt;
}

std::optional<std::string> VFS::ReadText(std::string_view path) const {
    if (auto bytes = Read(path)) {
        return std::string(bytes->begin(), bytes->end());
    }
    return std::nullopt;
}

}
