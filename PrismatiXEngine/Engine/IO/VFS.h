#pragma once

#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/SeekableStream.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::io {

class VFS {
public:
    // PrismatiX virtual paths are portable, package-relative UTF-8 paths.  All
    // callers (directory mounts, archives, scripts and assets) pass through
    // this grammar so a path cannot acquire different security semantics from
    // a different backing store.
    [[nodiscard]] static std::optional<std::string> NormalizeVirtualPath(
        std::string_view path);

    void MountDirectory(const std::string& root);
    bool MountArchive(const std::string& archivePath, const crypto::Key* key = nullptr);
    void Clear();

    [[nodiscard]] bool Exists(std::string_view path) const;
    // Opens an independent seekable cursor. Directory files and uncompressed,
    // unencrypted archive entries are streamed without retaining the full asset.
    [[nodiscard]] std::unique_ptr<SeekableReadStream> Open(
        std::string_view path) const;
    [[nodiscard]] std::optional<Bytes> Read(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> ReadText(std::string_view path) const;

private:
    std::vector<std::string> m_dirs;
    std::vector<std::unique_ptr<Archive>> m_archives;
};

}
