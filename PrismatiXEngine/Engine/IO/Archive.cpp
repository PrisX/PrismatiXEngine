#include "Engine/IO/Archive.h"

#include "Engine/Support/Logger.h"

#include <zstd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace px::io {

namespace {

constexpr char kMagic[4] = { 'P', 'D', 'X', '4' };
constexpr std::uint32_t kVersion = 4;
constexpr std::size_t kHeaderSize = 28;
constexpr std::uint8_t kEntryEncrypted = 0x01;
constexpr std::uint8_t kEntryCompressed = 0x02;
constexpr std::uint32_t kFlagEncrypted = 0x01;
constexpr char kIndexSalt[] = "__pdx4_index__";
constexpr std::uint32_t kMaxEntries = 1'000'000;
constexpr std::uint64_t kMaxIndexSize = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxEntrySize = 8ull * 1024ull * 1024ull * 1024ull;
constexpr std::size_t kMaxNameLength = 4096;
constexpr std::size_t kFixedEntryBytes = 8 + 2 + 8 + 8 + 8 + 4 + 1;

void PutU16(Bytes& b, std::uint16_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
}
void PutU32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (i * 8)) & 0xFF);
}
void PutU64(Bytes& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((v >> (i * 8)) & 0xFF);
}

struct Cursor {
    const std::uint8_t* p;
    const std::uint8_t* end;
    [[nodiscard]] std::size_t Remaining() const { return p <= end ? static_cast<std::size_t>(end - p) : 0; }
    [[nodiscard]] bool Ok(std::size_t n) const { return n <= Remaining(); }
    std::uint16_t U16() {
        std::uint16_t v = p[0] | (p[1] << 8);
        p += 2;
        return v;
    }
    std::uint32_t U32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
        p += 4;
        return v;
    }
    std::uint64_t U64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
        p += 8;
        return v;
    }
};

Bytes Compress(const Bytes& src) {
    const std::size_t bound = ZSTD_compressBound(src.size());
    Bytes out(bound);
    const std::size_t n = ZSTD_compress(out.data(), bound, src.data(), src.size(), 19);
    if (ZSTD_isError(n)) {
        return {};
    }
    out.resize(n);
    return out;
}

Bytes Decompress(const Bytes& src, std::size_t rawSize) {
    Bytes out(rawSize);
    const std::size_t n = ZSTD_decompress(out.data(), rawSize, src.data(), src.size());
    if (ZSTD_isError(n) || n != rawSize) {
        return {};
    }
    return out;
}

std::string NormalizePath(std::string_view path) {
    std::string s(path);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

bool IsSafeLogicalPath(std::string_view path) {
    if (path.empty() || path.size() > kMaxNameLength || path.front() == '/' ||
        path.find(':') != std::string_view::npos) return false;
    const std::filesystem::path parsed(path);
    if (parsed.is_absolute()) return false;
    for (const auto& component : parsed) {
        if (component == "." || component == ".." || component.empty()) return false;
    }
    return true;
}

}


bool Archive::Open(const std::string& path, const crypto::Key* key) {
    Close();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        PX_LOG_ERROR("Archive::Open cannot open '{}'", path);
        return false;
    }
    std::error_code sizeError;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize < kHeaderSize) {
        PX_LOG_ERROR("Archive::Open invalid file size for '{}'", path);
        return false;
    }

    std::uint8_t header[kHeaderSize];
    in.read(reinterpret_cast<char*>(header), kHeaderSize);
    if (!in || std::memcmp(header, kMagic, 4) != 0) {
        PX_LOG_ERROR("Archive::Open bad magic in '{}'", path);
        return false;
    }
    Cursor c{ header + 4, header + kHeaderSize };
    const std::uint32_t version = c.U32();
    const std::uint32_t flags = c.U32();
    const std::uint64_t indexOffset = c.U64();
    const std::uint64_t indexStored = c.U64();
    if (version != kVersion) {
        PX_LOG_ERROR("Archive::Open unsupported version {} in '{}'", version, path);
        return false;
    }
    if (indexStored == 0 || indexStored > kMaxIndexSize || indexStored > fileSize ||
        indexOffset < kHeaderSize || indexOffset > fileSize || indexStored > fileSize - indexOffset ||
        indexStored > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        indexStored > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
        PX_LOG_ERROR("Archive::Open invalid index bounds in '{}'", path);
        return false;
    }

    m_encrypted = (flags & kFlagEncrypted) != 0;
    if (m_encrypted) {
        if (!key) {
            PX_LOG_ERROR("Archive::Open '{}' is encrypted but no key supplied", path);
            return false;
        }
        m_key = *key;
    }

    Bytes indexBlob(indexStored);
    in.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    in.read(reinterpret_cast<char*>(indexBlob.data()), static_cast<std::streamsize>(indexStored));
    if (!in) {
        PX_LOG_ERROR("Archive::Open failed to read index of '{}'", path);
        return false;
    }
    if (m_encrypted) {
        indexBlob = crypto::Decrypt(indexBlob, m_key, crypto::DeriveIv(kIndexSalt));
        if (indexBlob.empty()) {
            PX_LOG_ERROR("Archive::Open index decrypt failed (wrong key?) for '{}'", path);
            return false;
        }
    }
    if (indexBlob.size() > kMaxIndexSize) return false;

    Cursor ic{ indexBlob.data(), indexBlob.data() + indexBlob.size() };
    if (!ic.Ok(4)) {
        return false;
    }
    const std::uint32_t count = ic.U32();
    if (count > kMaxEntries || static_cast<std::uint64_t>(count) * kFixedEntryBytes > ic.Remaining()) {
        PX_LOG_ERROR("Archive::Open invalid entry count in '{}'", path);
        return false;
    }
    m_entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry e;
        if (!ic.Ok(10)) return false;
        e.pathHash = ic.U64();
        const std::uint16_t nameLen = ic.U16();
        if (nameLen == 0 || nameLen > kMaxNameLength || !ic.Ok(static_cast<std::size_t>(nameLen) + 29)) {
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(ic.p), nameLen);
        ic.p += nameLen;
        e.offset = ic.U64();
        e.rawSize = ic.U64();
        e.storedSize = ic.U64();
        e.crc = ic.U32();
        e.flags = *ic.p++;
        if (NormalizePath(e.name) != e.name || !IsSafeLogicalPath(e.name) ||
            e.pathHash != crypto::HashPath(e.name) || (e.flags & ~(kEntryEncrypted | kEntryCompressed)) != 0 ||
            e.rawSize > kMaxEntrySize || e.storedSize > kMaxEntrySize ||
            e.rawSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
            e.storedSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
            e.storedSize > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()) ||
            e.offset < kHeaderSize || e.offset > indexOffset || e.storedSize > indexOffset - e.offset ||
            m_index.contains(e.pathHash)) {
            PX_LOG_ERROR("Archive::Open invalid or colliding entry in '{}'", path);
            return false;
        }
        m_index[e.pathHash] = m_entries.size();
        m_entries.push_back(std::move(e));
    }
    if (ic.Remaining() != 0) {
        PX_LOG_ERROR("Archive::Open trailing bytes in index of '{}'", path);
        return false;
    }

    m_path = path;
    m_open = true;
    PX_LOG_INFO("Mounted archive '{}' ({} entries, encrypted={})", path, m_entries.size(),
                m_encrypted);
    return true;
}

void Archive::Close() {
    m_open = false;
    m_encrypted = false;
    m_entries.clear();
    m_index.clear();
    m_path.clear();
}

bool Archive::Contains(std::string_view path) const {
    if (!m_open) return false;
    const std::string normalized = NormalizePath(path);
    const auto found = m_index.find(crypto::HashPath(normalized));
    return found != m_index.end() && found->second < m_entries.size() &&
           m_entries[found->second].name == normalized;
}

std::optional<Bytes> Archive::Read(std::string_view path) const {
    if (!m_open) {
        return std::nullopt;
    }
    const std::string normalized = NormalizePath(path);
    auto it = m_index.find(crypto::HashPath(normalized));
    if (it == m_index.end()) {
        return std::nullopt;
    }
    const Entry& e = m_entries[it->second];
    if (e.name != normalized) return std::nullopt;

    std::ifstream in(m_path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Bytes stored(e.storedSize);
    in.seekg(static_cast<std::streamoff>(e.offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(e.storedSize));
    if (!in) {
        return std::nullopt;
    }

    Bytes stage = std::move(stored);
    if (e.flags & kEntryEncrypted) {
        stage = crypto::Decrypt(stage, m_key, crypto::DeriveIv(e.name));
        if (stage.empty() && e.rawSize != 0) {
            return std::nullopt;
        }
    }
    Bytes raw =
        (e.flags & kEntryCompressed) ? Decompress(stage, e.rawSize) : std::move(stage);
    if (raw.size() != e.rawSize) {
        PX_LOG_ERROR("Archive::Read size mismatch for '{}'", e.name);
        return std::nullopt;
    }
    if (crypto::Crc32(raw.data(), raw.size()) != e.crc) {
        PX_LOG_ERROR("Archive::Read crc mismatch for '{}'", e.name);
        return std::nullopt;
    }
    return raw;
}


void ArchiveWriter::SetKey(const crypto::Key& key) {
    m_key = key;
    m_encrypt = true;
}

void ArchiveWriter::Add(const std::string& logicalPath, const Bytes& data) {
    m_pending.push_back({ NormalizePath(logicalPath), data });
}

bool ArchiveWriter::Write(const std::string& outPath) const {
    if (m_pending.size() > kMaxEntries) {
        PX_LOG_ERROR("ArchiveWriter::Write too many entries for '{}'", outPath);
        return false;
    }
    Bytes data;
    Bytes index;
    PutU32(index, static_cast<std::uint32_t>(m_pending.size()));
    std::unordered_set<std::uint64_t> hashes;

    for (const Pending& p : m_pending) {
        const std::uint64_t pathHash = crypto::HashPath(p.name);
        if (!IsSafeLogicalPath(p.name) || p.name.size() > (std::numeric_limits<std::uint16_t>::max)() ||
            p.data.size() > kMaxEntrySize || !hashes.insert(pathHash).second) {
            PX_LOG_ERROR("ArchiveWriter::Write invalid or colliding logical path '{}'", p.name);
            return false;
        }
        const std::uint32_t crc = crypto::Crc32(p.data.data(), p.data.size());
        std::uint8_t flags = 0;

        Bytes stage = p.data;
        if (m_compress && !p.data.empty()) {
            Bytes comp = Compress(p.data);
            if (!comp.empty() && comp.size() < p.data.size()) {
                stage = std::move(comp);
                flags |= kEntryCompressed;
            }
        }
        if (m_encrypt) {
            stage = crypto::Encrypt(stage, m_key, crypto::DeriveIv(p.name));
            flags |= kEntryEncrypted;
        }

        const std::uint64_t offset = kHeaderSize + data.size();
        const std::uint64_t storedSize = stage.size();
        data.insert(data.end(), stage.begin(), stage.end());

        PutU64(index, pathHash);
        PutU16(index, static_cast<std::uint16_t>(p.name.size()));
        index.insert(index.end(), p.name.begin(), p.name.end());
        PutU64(index, offset);
        PutU64(index, p.data.size());
        PutU64(index, storedSize);
        PutU32(index, crc);
        index.push_back(flags);
    }

    if (m_encrypt) {
        index = crypto::Encrypt(index, m_key, crypto::DeriveIv(kIndexSalt));
    }
    if (index.empty() || index.size() > kMaxIndexSize) {
        PX_LOG_ERROR("ArchiveWriter::Write index is too large for '{}'", outPath);
        return false;
    }

    const std::uint64_t indexOffset = kHeaderSize + data.size();

    Bytes header;
    header.insert(header.end(), kMagic, kMagic + 4);
    PutU32(header, kVersion);
    PutU32(header, m_encrypt ? kFlagEncrypted : 0u);
    PutU64(header, indexOffset);
    PutU64(header, index.size());

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        PX_LOG_ERROR("ArchiveWriter::Write cannot open '{}'", outPath);
        return false;
    }
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.write(reinterpret_cast<const char*>(index.data()),
              static_cast<std::streamsize>(index.size()));
    if (!out) {
        PX_LOG_ERROR("ArchiveWriter::Write failed writing '{}'", outPath);
        return false;
    }
    PX_LOG_INFO("Wrote archive '{}' ({} entries, encrypted={})", outPath, m_pending.size(),
                m_encrypt);
    return true;
}

}
