#include "Engine/IO/VFS.h"

#include "Engine/Support/Logger.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>

namespace px::io {

namespace {
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
