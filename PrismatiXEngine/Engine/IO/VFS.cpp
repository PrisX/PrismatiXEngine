#include "Engine/IO/VFS.h"

#include "Engine/Support/Logger.h"

#include <cstdint>
#include <limits>
#include <mutex>

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

void VFS::MountDirectory(const std::string& root,
                         std::shared_ptr<const FileSource> source) {
    if (!source) source = PackageFileSource();
    auto canonical = source->CanonicalRoot(root);
    if (!canonical) {
        PX_LOG_ERROR("VFS rejected invalid directory mount '{}'", root);
        return;
    }
    PX_LOG_INFO("VFS mounted directory '{}' (source={})", *canonical,
                source->Name());
    std::unique_lock lock(m_mountsMutex);
    m_dirs.push_back({std::move(*canonical), std::move(source)});
}

void VFS::MountFallbackDirectory(const std::string& root,
                                 std::shared_ptr<const FileSource> source) {
    if (!source) source = PackageFileSource();
    auto canonical = source->CanonicalRoot(root);
    if (!canonical) {
        PX_LOG_ERROR("VFS rejected invalid fallback directory '{}'", root);
        return;
    }
    PX_LOG_INFO("VFS mounted fallback directory '{}' (source={})", *canonical,
                source->Name());
    std::unique_lock lock(m_mountsMutex);
    m_fallbackDirs.push_back({std::move(*canonical), std::move(source)});
}

bool VFS::MountArchive(const std::string& archivePath, const crypto::Key* key,
                       const ArchiveMountPriority priority,
                       const std::vector<std::string>& tombstones,
                       std::shared_ptr<const FileSource> source) {
#if defined(PRISMATIX_PREVIEW_WASM)
    (void)source;
    (void)archivePath;
    (void)key;
    (void)priority;
    (void)tombstones;
    PX_LOG_ERROR("Archive mounts are unavailable in the WASM preview VFS");
    return false;
#else
    auto archive = std::make_unique<Archive>();
    if (!archive->Open(std::move(source), archivePath, key)) {
        return false;
    }
    if ((priority != ArchiveMountPriority::Overlay && !tombstones.empty()) ||
        tombstones.size() > 1'000'000) {
        PX_LOG_ERROR("VFS rejected tombstones on an invalid archive mount '{}'", archivePath);
        return false;
    }
    std::unordered_set<std::string> deleted;
    deleted.reserve(tombstones.size());
    for (const auto& path : tombstones) {
        const auto normalized = NormalizeVirtualPath(path);
        if (!normalized || *normalized != path || path.size() > 4096 ||
            archive->Contains(path) || !deleted.insert(path).second) {
            PX_LOG_ERROR("VFS rejected invalid or conflicting tombstone '{}'", path);
            return false;
        }
    }
    std::unique_lock lock(m_mountsMutex);
    ArchiveMount mount{std::move(archive), std::move(deleted)};
    if (priority == ArchiveMountPriority::Overlay) {
        m_archives.insert(m_archives.begin(), std::move(mount));
    } else {
        m_archives.push_back(std::move(mount));
    }
    return true;
#endif
}

void VFS::Clear() {
    std::unique_lock lock(m_mountsMutex);
    m_dirs.clear();
    m_archives.clear();
    m_fallbackDirs.clear();
}

bool VFS::Exists(std::string_view path) const {
    const auto normalized = NormalizeVirtualPath(path);
    if (!normalized) return false;
    std::shared_lock lock(m_mountsMutex);
    for (const auto& dir : m_dirs) {
        const auto full = dir.source->ResolveUnder(dir.root, *normalized);
        if (full && dir.source->IsFile(*full)) return true;
    }
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& mount : m_archives) {
        if (mount.tombstones.contains(*normalized)) return false;
        if (mount.archive->Contains(*normalized)) return true;
    }
#endif
    for (const auto& dir : m_fallbackDirs) {
        const auto full = dir.source->ResolveUnder(dir.root, *normalized);
        if (full && dir.source->IsFile(*full)) return true;
    }
    return false;
}

std::unique_ptr<SeekableReadStream> VFS::Open(const std::string_view path) const {
    const auto normalized = NormalizeVirtualPath(path);
    if (!normalized) return {};
    std::shared_lock lock(m_mountsMutex);
    for (const auto& dir : m_dirs) {
        const auto full = dir.source->ResolveUnder(dir.root, *normalized);
        if (!full || !dir.source->IsFile(*full)) continue;
        if (auto stream = dir.source->Open(*full)) return stream;
    }
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& mount : m_archives) {
        if (mount.tombstones.contains(*normalized)) return {};
        if (mount.archive->Contains(*normalized))
            return mount.archive->OpenStream(*normalized);
    }
#endif
    for (const auto& dir : m_fallbackDirs) {
        const auto full = dir.source->ResolveUnder(dir.root, *normalized);
        if (!full || !dir.source->IsFile(*full)) continue;
        if (auto stream = dir.source->Open(*full)) return stream;
    }
    return {};
}

std::optional<Bytes> VFS::Read(std::string_view path) const {
    const auto normalized = NormalizeVirtualPath(path);
    if (!normalized) return std::nullopt;
    std::shared_lock lock(m_mountsMutex);
    for (const auto& dir : m_dirs) {
        const auto full = dir.source->ResolveUnder(dir.root, *normalized);
        if (!full) continue;
        if (auto data = ReadFile(*dir.source, *full,
                                 (std::numeric_limits<std::size_t>::max)()))
            return data;
    }
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& mount : m_archives) {
        if (mount.tombstones.contains(*normalized)) return std::nullopt;
        if (mount.archive->Contains(*normalized))
            return mount.archive->Read(*normalized);
    }
#endif
    for (const auto& dir : m_fallbackDirs) {
        const auto full = dir.source->ResolveUnder(dir.root, *normalized);
        if (!full) continue;
        if (auto data = ReadFile(*dir.source, *full,
                                 (std::numeric_limits<std::size_t>::max)()))
            return data;
    }
    return std::nullopt;
}

std::optional<std::string> VFS::ReadText(std::string_view path) const {
    if (auto bytes = Read(path)) {
        return std::string(bytes->begin(), bytes->end());
    }
    return std::nullopt;
}

}
