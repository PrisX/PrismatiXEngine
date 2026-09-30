#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/IO/FileSource.h"
#include "Engine/IO/SeekableStream.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::io {

using Bytes = std::vector<std::uint8_t>;

class Archive {
public:
    struct Entry {
        std::uint64_t pathHash = 0;
        std::string name;
        std::uint64_t offset = 0;
        std::uint64_t rawSize = 0;
        std::uint64_t storedSize = 0;
        std::uint32_t crc = 0;
        std::uint8_t flags = 0;
        std::uint32_t chunkSize = 0;
        std::uint32_t chunkCount = 0;
    };

    // Reads through PackageFileSource(): the file system on desktop, APK
    // assets and internal storage on Android.
    bool Open(const std::string& path, const crypto::Key* key = nullptr);
    // Every stream opened from this archive reads `path` through `source`,
    // each with its own cursor. A null source means PackageFileSource().
    bool Open(std::shared_ptr<const FileSource> source, const std::string& path,
              const crypto::Key* key = nullptr);
    void Close();

    [[nodiscard]] bool IsOpen() const { return m_open; }
    [[nodiscard]] bool Contains(std::string_view path) const;
    // Unencrypted compressed entries decode with a bounded zstd window;
    // size/CRC validation completes when the stream is read to the end.
    // Legacy whole-entry encrypted+compressed entries still buffer fully.
    [[nodiscard]] std::unique_ptr<SeekableReadStream> OpenStream(
        std::string_view path) const;
    // Package verification checks SHA-256 and CRC in the same full read.
    // Only that verifier should use this entry point: plain entries skip the
    // eager CRC pass performed by OpenStream().
    [[nodiscard]] std::unique_ptr<SeekableReadStream> OpenStreamForVerification(
        std::string_view path) const;
    [[nodiscard]] std::optional<Bytes> Read(std::string_view path) const;
    [[nodiscard]] const std::vector<Entry>& Entries() const { return m_entries; }

private:
    [[nodiscard]] std::unique_ptr<SeekableReadStream> OpenStreamImpl(
        std::string_view path, bool preverifyPlainCrc) const;
    std::string m_path;
    std::shared_ptr<const FileSource> m_source;
    bool m_open = false;
    bool m_encrypted = false;
    std::uint32_t m_version = 0;
    crypto::Key m_key{};
    std::vector<Entry> m_entries;
    std::unordered_map<std::uint64_t, std::size_t> m_index;
};

class ArchiveWriter {
public:
    void SetKey(const crypto::Key& key);
    void SetCompression(bool enabled) { m_compress = enabled; }

    void Add(const std::string& logicalPath, const Bytes& data);
    [[nodiscard]] bool Write(const std::string& outPath) const;

private:
    struct Pending {
        std::string name;
        Bytes data;
    };
    std::vector<Pending> m_pending;
    bool m_encrypt = false;
    bool m_compress = true;
    crypto::Key m_key{};
};

// Writes unencrypted PDX5 entries without buffering file-backed resources in
// memory. File and stream sources may use bounded streaming zstd compression.
// Their expected size and SHA-256 make a changing input fail during the write.
class ArchiveStreamWriter {
public:
    using Progress = std::function<void(std::uint64_t current,
                                        std::uint64_t total)>;
    using StreamFactory =
        std::function<std::unique_ptr<SeekableReadStream>()>;
    void AddFile(std::string logicalPath, std::filesystem::path source,
                 std::uint64_t expectedSize, std::string expectedSha256,
                 bool compress = false);
    void AddStream(std::string logicalPath, StreamFactory source,
                   std::uint64_t expectedSize, std::string expectedSha256,
                   bool compress = false);
    void AddData(std::string logicalPath, Bytes data);
    [[nodiscard]] bool Write(const std::string& outPath,
                             const Progress& onProgress = {}) const;

private:
    enum class SourceKind { Data, File, Stream };
    struct Pending {
        std::string name;
        std::filesystem::path source;
        Bytes data;
        StreamFactory stream;
        std::uint64_t expectedSize = 0;
        std::string expectedSha256;
        SourceKind kind = SourceKind::Data;
        bool compress = false;
    };
    std::vector<Pending> m_pending;
};

}
