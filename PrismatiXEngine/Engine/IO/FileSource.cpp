#include "Engine/IO/FileSource.h"

#include "Engine/Support/Utf8Path.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <unordered_set>

namespace px::io {

#if defined(PRISMATIX_WEB_PLAYER)
std::shared_ptr<const FileSource> WebFileSource();
#endif

namespace {

// Resolves a cursor move against [0, size]. Shared by both stream types so
// they agree on every boundary the SeekableReadStream contract defines.
std::optional<std::uint64_t> SeekTarget(const std::uint64_t position,
                                        const std::uint64_t size,
                                        const std::int64_t offset,
                                        const SeekOrigin origin) {
    constexpr auto kMax = (std::numeric_limits<std::int64_t>::max)();
    constexpr auto kMin = (std::numeric_limits<std::int64_t>::min)();
    std::uint64_t unsignedBase = 0;
    if (origin == SeekOrigin::Current) unsignedBase = position;
    else if (origin == SeekOrigin::End) unsignedBase = size;
    if (unsignedBase > static_cast<std::uint64_t>(kMax)) return std::nullopt;
    const auto base = static_cast<std::int64_t>(unsignedBase);
    if ((offset > 0 && base > kMax - offset) ||
        (offset < 0 && base < kMin - offset))
        return std::nullopt;
    const std::int64_t requested = base + offset;
    if (requested < 0 || static_cast<std::uint64_t>(requested) > size)
        return std::nullopt;
    return static_cast<std::uint64_t>(requested);
}

class NativeReadStream final : public SeekableReadStream {
public:
    explicit NativeReadStream(const std::filesystem::path& path)
        : m_input(path, std::ios::binary | std::ios::ate) {
        if (!m_input) {
            m_failed = true;
            return;
        }
        // Size from the open handle: no second path lookup, and the size
        // cannot disagree with the file this cursor actually reads.
        const std::streamoff end = m_input.tellg();
        m_input.seekg(0, std::ios::beg);
        if (end < 0 || !m_input) {
            m_failed = true;
            return;
        }
        m_size = static_cast<std::uint64_t>(end);
    }

    [[nodiscard]] std::size_t Read(std::uint8_t* destination,
                                   const std::size_t bytes) override {
        if (m_failed || !destination || bytes == 0 || m_position >= m_size)
            return 0;
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
            m_size - m_position,
            std::min<std::uint64_t>(
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
        const auto target = SeekTarget(m_position, m_size, offset, origin);
        if (!target ||
            *target > static_cast<std::uint64_t>(
                          (std::numeric_limits<std::streamoff>::max)()))
            return false;
        m_input.clear();
        m_input.seekg(static_cast<std::streamoff>(*target), std::ios::beg);
        if (!m_input) {
            m_failed = true;
            return false;
        }
        m_position = *target;
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

class SdlReadStream final : public SeekableReadStream {
public:
    explicit SdlReadStream(SDL_IOStream* io) : m_io(io) {
        const Sint64 size = m_io ? SDL_GetIOSize(m_io) : -1;
        if (size < 0) {
            m_failed = true;
            return;
        }
        m_size = static_cast<std::uint64_t>(size);
    }

    ~SdlReadStream() override {
        if (m_io) SDL_CloseIO(m_io);
    }

    SdlReadStream(const SdlReadStream&) = delete;
    SdlReadStream& operator=(const SdlReadStream&) = delete;

    [[nodiscard]] std::size_t Read(std::uint8_t* destination,
                                   const std::size_t bytes) override {
        if (m_failed || !destination || bytes == 0 || m_position >= m_size)
            return 0;
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(m_size - m_position, bytes));
        const std::size_t read = SDL_ReadIO(m_io, destination, count);
        // Short reads are legal; a read that produces nothing before the
        // known end, or an explicit error status, means the backing changed.
        if (read == 0 || SDL_GetIOStatus(m_io) == SDL_IO_STATUS_ERROR) {
            m_failed = true;
            if (read == 0) return 0;
        }
        m_position += read;
        return read;
    }

    [[nodiscard]] bool Seek(const std::int64_t offset,
                            const SeekOrigin origin) override {
        if (m_failed) return false;
        const auto target = SeekTarget(m_position, m_size, offset, origin);
        if (!target) return false;
        if (SDL_SeekIO(m_io, static_cast<Sint64>(*target), SDL_IO_SEEK_SET) !=
            static_cast<Sint64>(*target)) {
            m_failed = true;
            return false;
        }
        m_position = *target;
        return true;
    }

    [[nodiscard]] std::uint64_t Tell() const override { return m_position; }
    [[nodiscard]] std::uint64_t Size() const override { return m_size; }
    [[nodiscard]] bool Failed() const override { return m_failed; }

private:
    SDL_IOStream* m_io = nullptr;
    std::uint64_t m_size = 0;
    std::uint64_t m_position = 0;
    bool m_failed = false;
};

class NativeFileSourceImpl final : public FileSource {
public:
    [[nodiscard]] std::unique_ptr<SeekableReadStream> Open(
        const std::string_view path) const override {
        auto stream = std::make_unique<NativeReadStream>(px::PathFromUtf8(path));
        if (stream->Failed()) return {};
        return stream;
    }

    [[nodiscard]] std::optional<std::uint64_t> FileSize(
        const std::string_view path) const override {
        std::error_code error;
        const auto native = px::PathFromUtf8(path);
        if (!std::filesystem::is_regular_file(native, error) || error)
            return std::nullopt;
        const auto size = std::filesystem::file_size(native, error);
        if (error) return std::nullopt;
        return static_cast<std::uint64_t>(size);
    }

    [[nodiscard]] std::optional<std::vector<std::string>> ListFiles(
        const std::string_view directory) const override {
        std::error_code error;
        std::filesystem::directory_iterator iterator(px::PathFromUtf8(directory),
                                                     error);
        if (error) return std::nullopt;
        std::vector<std::string> names;
        for (const std::filesystem::directory_iterator end; iterator != end;
             iterator.increment(error)) {
            if (error) return std::nullopt;
            std::error_code statusError;
            if (iterator->is_regular_file(statusError) && !statusError)
                names.push_back(px::PathToUtf8(iterator->path().filename()));
        }
        if (error) return std::nullopt;
        std::ranges::sort(names);
        return names;
    }

    [[nodiscard]] std::optional<std::string> CanonicalRoot(
        const std::string_view root) const override {
        std::error_code error;
        const auto canonical =
            std::filesystem::weakly_canonical(px::PathFromUtf8(root), error);
        if (error || canonical.empty()) return std::nullopt;
        return px::PathToUtf8(canonical);
    }

    // Canonicalizes the joined path so a symlink inside a mounted directory
    // cannot point the VFS outside the mount. Most lookups take a shorter
    // route to the same answer (PlainUnder).
    [[nodiscard]] std::optional<std::string> ResolveUnder(
        const std::string_view root,
        const std::string_view relative) const override {
        if (auto plain = PlainUnder(root, relative)) return plain;
        std::error_code error;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(px::PathFromUtf8(root), error);
        if (error) return std::nullopt;
        const std::filesystem::path candidate = std::filesystem::weakly_canonical(
            canonicalRoot / px::PathFromUtf8(relative), error);
        if (error) return std::nullopt;
        const std::filesystem::path within =
            std::filesystem::relative(candidate, canonicalRoot, error);
        if (error || within.empty() || within.is_absolute()) return std::nullopt;
        const auto first = within.begin();
        if (first == within.end() || *first == "..") return std::nullopt;
        return px::PathToUtf8(candidate);
    }

    [[nodiscard]] std::string_view Name() const override { return "native"; }

private:
    // Below a canonical root, a path whose directories are plain directories
    // (no symlink or junction) and whose last part is a plain file, or whose
    // first missing part ends the walk, cannot leave the root and is already
    // canonical. A game read from directory mounts (`prismatix run --dev`)
    // resolves every file it opens, and two weakly_canonical calls each time
    // added over 10 s to a large game's startup. Directories found plain are
    // remembered for the process. nullopt: ask weakly_canonical instead.
    [[nodiscard]] std::optional<std::string> PlainUnder(
        const std::string_view root, const std::string_view relative) const {
        const auto joined = px::PathFromUtf8(relative);
        const std::vector<std::filesystem::path> parts(joined.begin(), joined.end());
        if (parts.empty()) return std::nullopt;
        auto path = px::PathFromUtf8(root);
        const auto whole = [&] { return px::PathToUtf8((px::PathFromUtf8(root) / joined).make_preferred()); };
        std::error_code error;
        for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
            path /= parts[index];
            auto key = px::PathToUtf8(path);
            {
                std::scoped_lock lock(m_plainMutex);
                if (m_plainDirectories.contains(key)) continue;
            }
            const auto type = std::filesystem::symlink_status(path, error).type();
            if (type == std::filesystem::file_type::not_found) return whole();
            if (error || type != std::filesystem::file_type::directory) return std::nullopt;
            std::scoped_lock lock(m_plainMutex);
            m_plainDirectories.insert(std::move(key));
        }
        path /= parts.back();
        const auto type = std::filesystem::symlink_status(path, error).type();
        if (type == std::filesystem::file_type::regular ||
            type == std::filesystem::file_type::not_found)
            return px::PathToUtf8(path);
        return std::nullopt;
    }

    mutable std::mutex m_plainMutex;
    mutable std::unordered_set<std::string> m_plainDirectories;
};

class SdlFileSourceImpl final : public FileSource {
public:
    [[nodiscard]] std::unique_ptr<SeekableReadStream> Open(
        const std::string_view path) const override {
        const std::string text(path);
        SDL_IOStream* io = SDL_IOFromFile(text.c_str(), "rb");
        if (!io) return {};
        auto stream = std::make_unique<SdlReadStream>(io);
        if (stream->Failed()) return {};
        return stream;
    }

    [[nodiscard]] std::optional<std::uint64_t> FileSize(
        const std::string_view path) const override {
        const std::string text(path);
        SDL_PathInfo info{};
        if (SDL_GetPathInfo(text.c_str(), &info)) {
            if (info.type != SDL_PATHTYPE_FILE) return std::nullopt;
            return static_cast<std::uint64_t>(info.size);
        }
        // Path queries do not see APK assets on every Android release that
        // SDL supports; opening the asset is the authoritative check there.
        SDL_IOStream* io = SDL_IOFromFile(text.c_str(), "rb");
        if (!io) return std::nullopt;
        const Sint64 size = SDL_GetIOSize(io);
        SDL_CloseIO(io);
        if (size < 0) return std::nullopt;
        return static_cast<std::uint64_t>(size);
    }

    [[nodiscard]] std::optional<std::vector<std::string>> ListFiles(
        const std::string_view directory) const override {
        const std::string text(directory);
        std::vector<std::string> entries;
        const auto collect = [](void* userdata, const char*,
                                const char* name) -> SDL_EnumerationResult {
            static_cast<std::vector<std::string>*>(userdata)->emplace_back(name);
            return SDL_ENUM_CONTINUE;
        };
        if (!SDL_EnumerateDirectory(text.c_str(), collect, &entries))
            return std::nullopt;
        std::vector<std::string> names;
        for (auto& name : entries) {
            const std::string child =
                text.empty() || text.back() == '/' ? text + name : text + '/' + name;
            if (IsFile(child)) names.push_back(std::move(name));
        }
        std::ranges::sort(names);
        return names;
    }

    // APK assets have no symlinks and VFS paths cannot contain "..", so a
    // lexical join is exact. "." and "./" name the asset root.
    [[nodiscard]] std::optional<std::string> CanonicalRoot(
        const std::string_view root) const override {
        std::string_view trimmed = root;
        while (trimmed.starts_with("./")) trimmed.remove_prefix(2);
        while (trimmed.size() > 1 && trimmed.ends_with('/'))
            trimmed.remove_suffix(1);
        if (trimmed == ".") trimmed = {};
        return std::string(trimmed);
    }

    [[nodiscard]] std::optional<std::string> ResolveUnder(
        const std::string_view root,
        const std::string_view relative) const override {
        if (relative.empty()) return std::nullopt;
        if (root.empty()) return std::string(relative);
        std::string joined(root);
        if (joined.back() != '/') joined += '/';
        joined += relative;
        return joined;
    }

    [[nodiscard]] std::string_view Name() const override { return "sdl"; }
};

}  // namespace

class ListedFileSourceImpl final : public FileSource {
public:
    [[nodiscard]] std::unique_ptr<SeekableReadStream> Open(
        const std::string_view path) const override {
        return NativeFileSource()->Open(path);
    }

    [[nodiscard]] std::optional<std::uint64_t> FileSize(
        const std::string_view path) const override {
        const auto native = px::PathFromUtf8(path);
        const auto entry = Find(native.parent_path(), native.filename());
        if (!entry || entry->type != std::filesystem::file_type::regular)
            return std::nullopt;
        return entry->size;
    }

    [[nodiscard]] std::optional<std::vector<std::string>> ListFiles(
        const std::string_view directory) const override {
        const auto listing = List(px::PathFromUtf8(directory));
        if (!listing) return std::nullopt;
        std::vector<std::string> names;
        for (const auto& [key, entry] : *listing)
            if (entry.type == std::filesystem::file_type::regular)
                names.push_back(entry.name);
        std::ranges::sort(names);
        return names;
    }

    [[nodiscard]] std::optional<std::string> CanonicalRoot(
        const std::string_view root) const override {
        return NativeFileSource()->CanonicalRoot(root);
    }

    // Walks the listings from the canonical root: plain directories down to a
    // plain file (or the first missing part) cannot leave the root. Anything
    // else (a link, a junction) gets the native, canonicalizing check.
    [[nodiscard]] std::optional<std::string> ResolveUnder(
        const std::string_view root, const std::string_view relative) const override {
        const auto joined = px::PathFromUtf8(relative);
        const std::vector<std::filesystem::path> parts(joined.begin(), joined.end());
        if (parts.empty()) return std::nullopt;
        auto path = px::PathFromUtf8(root);
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const auto entry = Find(path, parts[index]);
            if (!entry) return px::PathToUtf8((px::PathFromUtf8(root) / joined).make_preferred());
            const bool last = index + 1 == parts.size();
            const auto wanted = last ? std::filesystem::file_type::regular
                                     : std::filesystem::file_type::directory;
            if (entry->type != wanted)
                return NativeFileSource()->ResolveUnder(root, relative);
            path /= parts[index];
        }
        return px::PathToUtf8(path.make_preferred());
    }

    [[nodiscard]] std::string_view Name() const override { return "listed"; }

private:
    struct Entry {
        std::string name;
        std::filesystem::file_type type = std::filesystem::file_type::unknown;
        std::uint64_t size = 0;
    };
    using Listing = std::unordered_map<std::string, Entry>;

    // Windows names match without regard to case, as the file system does.
    static std::string Key(const std::filesystem::path& name) {
#if defined(_WIN32)
        std::wstring folded = name.native();
        for (auto& character : folded)
            character = static_cast<wchar_t>(std::towlower(character));
        return px::PathToUtf8(std::filesystem::path(folded));
#else
        return px::PathToUtf8(name);
#endif
    }

    [[nodiscard]] std::shared_ptr<const Listing> List(const std::filesystem::path& directory) const {
        const auto key = Key(directory.lexically_normal());
        {
            std::scoped_lock lock(m_mutex);
            if (const auto found = m_listings.find(key); found != m_listings.end())
                return found->second;
        }
        std::shared_ptr<Listing> listing;
        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        if (!error) {
            listing = std::make_shared<Listing>();
            for (const std::filesystem::directory_iterator end; iterator != end;
                 iterator.increment(error)) {
                if (error) break;
                std::error_code entryError;
                Entry entry{px::PathToUtf8(iterator->path().filename()),
                            iterator->symlink_status(entryError).type(), 0};
                if (entryError) entry.type = std::filesystem::file_type::unknown;
                if (entry.type == std::filesystem::file_type::regular) {
                    entry.size = iterator->file_size(entryError);
                    if (entryError) entry.type = std::filesystem::file_type::unknown;
                }
                listing->emplace(Key(iterator->path().filename()), std::move(entry));
            }
            if (error) listing.reset();
        }
        std::scoped_lock lock(m_mutex);
        return m_listings.emplace(key, std::move(listing)).first->second;
    }

    [[nodiscard]] std::optional<Entry> Find(const std::filesystem::path& directory,
                                            const std::filesystem::path& name) const {
        const auto listing = List(directory);
        if (!listing) return std::nullopt;
        const auto found = listing->find(Key(name));
        if (found == listing->end()) return std::nullopt;
        return found->second;
    }

    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::shared_ptr<const Listing>> m_listings;
};

std::shared_ptr<const FileSource> NativeFileSource() {
    static const auto source = std::make_shared<const NativeFileSourceImpl>();
    return source;
}

std::shared_ptr<const FileSource> SdlFileSource() {
    static const auto source = std::make_shared<const SdlFileSourceImpl>();
    return source;
}

std::shared_ptr<const FileSource> ListedFileSource() {
    return std::make_shared<const ListedFileSourceImpl>();
}

std::shared_ptr<const FileSource> PackageFileSource() {
#if defined(PRISMATIX_WEB_PLAYER)
    return WebFileSource();
#elif defined(__ANDROID__)
    return SdlFileSource();
#else
    return NativeFileSource();
#endif
}

std::size_t ReadUpTo(SeekableReadStream& stream, std::uint8_t* destination,
                     const std::size_t bytes) {
    std::size_t total = 0;
    while (total < bytes) {
        const std::size_t read = stream.Read(destination + total, bytes - total);
        if (read == 0) break;
        total += read;
    }
    return total;
}

bool ReadExact(SeekableReadStream& stream, std::uint8_t* destination,
               const std::size_t bytes) {
    return ReadUpTo(stream, destination, bytes) == bytes && !stream.Failed();
}

std::optional<std::vector<std::uint8_t>> ReadFile(const FileSource& source,
                                                  const std::string_view path,
                                                  const std::uint64_t limit) {
    const auto size = source.FileSize(path);
    if (!size || *size > limit) return std::nullopt;
    auto stream = source.Open(path);
    if (!stream) return std::nullopt;
    const std::uint64_t length = stream->Size();
    if (length > limit ||
        length > static_cast<std::uint64_t>(
                     (std::numeric_limits<std::size_t>::max)()))
        return std::nullopt;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    if (!data.empty() && !ReadExact(*stream, data.data(), data.size()))
        return std::nullopt;
    return data;
}

std::optional<std::string> ReadTextFile(const FileSource& source,
                                        const std::string_view path,
                                        const std::uint64_t limit) {
    auto bytes = ReadFile(source, path, limit);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

}  // namespace px::io
