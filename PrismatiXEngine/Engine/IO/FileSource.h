#pragma once

#include "Engine/IO/SeekableStream.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::io {

// A read-only place that package files come from. Desktop builds read loose
// files and PDX shards straight from the file system. Android ships them
// inside the APK or a Play asset pack, which std::ifstream and
// std::filesystem cannot see, so every package reader (Archive, VFS
// directory mounts, the Player's boot manifest) opens files through this
// interface instead. Writes (saves, settings, AtomicFile) stay on the file
// system and do not go through a FileSource.
//
// Paths are UTF-8. Every stream returned by Open() owns an independent
// cursor, and a FileSource is safe to use from several threads at once.
class FileSource {
public:
    virtual ~FileSource() = default;

    // Opens a file for reading, or returns nullptr when it cannot be opened.
    // Call FileSize()/IsFile() first when `path` might name a directory.
    [[nodiscard]] virtual std::unique_ptr<SeekableReadStream> Open(
        std::string_view path) const = 0;
    // Size of a regular file; nullopt when the path is missing or is not a
    // regular file.
    [[nodiscard]] virtual std::optional<std::uint64_t> FileSize(
        std::string_view path) const = 0;
    // Names (not paths) of the regular files directly inside `directory`,
    // sorted by byte value; nullopt when the directory cannot be listed.
    [[nodiscard]] virtual std::optional<std::vector<std::string>> ListFiles(
        std::string_view directory) const = 0;
    // Mount support for VFS directory mounts. CanonicalRoot() turns a mount
    // root into the stable form that ResolveUnder() expects; ResolveUnder()
    // joins an already normalized package-relative path under it and returns
    // nullopt when the result would leave the root.
    [[nodiscard]] virtual std::optional<std::string> CanonicalRoot(
        std::string_view root) const = 0;
    [[nodiscard]] virtual std::optional<std::string> ResolveUnder(
        std::string_view canonicalRoot, std::string_view relative) const = 0;
    // Short, stable name for logs ("native", "sdl").
    [[nodiscard]] virtual std::string_view Name() const = 0;

    [[nodiscard]] bool IsFile(const std::string_view path) const {
        return FileSize(path).has_value();
    }
};

// std::ifstream/std::filesystem. This is what every desktop build used before
// the abstraction existed, with the same syscalls per read.
[[nodiscard]] std::shared_ptr<const FileSource> NativeFileSource();
// SDL_IOStream. On Android SDL_IOFromFile opens absolute paths from the file
// system and relative paths from internal storage first, then from the APK's
// assets (which include install-time Play asset packs). On desktop it opens
// ordinary files, which is how the tests exercise it.
[[nodiscard]] std::shared_ptr<const FileSource> SdlFileSource();
// The source shipped package files are read from: SdlFileSource() on
// Android, NativeFileSource() everywhere else.
[[nodiscard]] std::shared_ptr<const FileSource> PackageFileSource();
// Native files in folders that do not change while the game runs (a
// development run's artifact and project folder): each directory is listed
// once, and later lookups, sizes and link checks come from that listing.
// Asking the file system for each of a large game's tens of thousands of
// resource references took seconds. A file added after its directory was
// listed is not seen until the next run. Each call returns a new cache.
[[nodiscard]] std::shared_ptr<const FileSource> ListedFileSource();

// Reads until `bytes` have been copied or the stream stops producing data.
// Returns the number of bytes copied.
[[nodiscard]] std::size_t ReadUpTo(SeekableReadStream& stream,
                                   std::uint8_t* destination,
                                   std::size_t bytes);
// True only when exactly `bytes` were read.
[[nodiscard]] bool ReadExact(SeekableReadStream& stream,
                             std::uint8_t* destination, std::size_t bytes);
// Reads a whole regular file of at most `limit` bytes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadFile(
    const FileSource& source, std::string_view path,
    std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)());
[[nodiscard]] std::optional<std::string> ReadTextFile(
    const FileSource& source, std::string_view path,
    std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)());

}  // namespace px::io
