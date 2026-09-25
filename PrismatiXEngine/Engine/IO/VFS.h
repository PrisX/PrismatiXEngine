#pragma once

#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/SeekableStream.h"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace px::io {

enum class ArchiveMountPriority {
    // Preserve the existing first-mounted-archive-wins lookup order.
    Base,
    // A verified caller may override earlier archive entries. Directory
    // mounts still take precedence over every archive.
    Overlay,
};

class VFS {
public:
    // PrismatiX virtual paths are portable, package-relative UTF-8 paths.  All
    // callers (directory mounts, archives, scripts and assets) pass through
    // this grammar so a path cannot acquire different security semantics from
    // a different backing store.
    [[nodiscard]] static std::optional<std::string> NormalizeVirtualPath(
        std::string_view path);

    void MountDirectory(const std::string& root);
    // Tombstones are valid only for explicit overlays. They hide a virtual
    // path from every lower-priority archive; the caller must first verify
    // the patch identity and content. An overlay cannot both contain and
    // delete the same path.
    bool MountArchive(const std::string& archivePath,
                      const crypto::Key* key = nullptr,
                      ArchiveMountPriority priority = ArchiveMountPriority::Base,
                      const std::vector<std::string>& tombstones = {});
    void Clear();

    [[nodiscard]] bool Exists(std::string_view path) const;
    // Opens an independent seekable cursor. Concurrent reads are safe while
    // mounts are changed; an already opened cursor remains usable after Clear.
    // Directory files and uncompressed, unencrypted archive entries are
    // streamed without retaining the full asset.
    [[nodiscard]] std::unique_ptr<SeekableReadStream> Open(
        std::string_view path) const;
    [[nodiscard]] std::optional<Bytes> Read(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> ReadText(std::string_view path) const;

private:
    struct ArchiveMount {
        std::unique_ptr<Archive> archive;
        std::unordered_set<std::string> tombstones;
    };

    // Asset preloads may open independent streams on worker threads while a
    // preview session changes mounts on the main thread.
    mutable std::shared_mutex m_mountsMutex;
    std::vector<std::string> m_dirs;
    std::vector<ArchiveMount> m_archives;
};

}
