#include "Engine/Support/Utf8Path.h"
#include "Engine/SDK/Packager.h"

#include "Engine/Package/PackageManifest.h"

#include <psa/crypto.h>
#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/Core/SemanticVersion.h"
#include "Engine/SDK/Ui.h"
#include "Engine/SDK/CharacterResources.h"
#include "Engine/SDK/GameCatalogResources.h"

namespace px::sdk {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;
using Key = std::array<std::uint8_t, 32>;
using Iv = std::array<std::uint8_t, 16>;

constexpr char kArchiveMagic[4] = { 'P', 'D', 'X', '5' };
constexpr std::uint32_t kArchiveVersion = 5;
constexpr std::size_t kArchiveHeaderSize = 28;
constexpr std::uint8_t kEntryEncrypted = 0x01;
constexpr std::uint8_t kEntryCompressed = 0x02;
constexpr std::uint8_t kEntryChunkedEncrypted = 0x04;
constexpr std::uint32_t kArchiveEncrypted = 0x01;
constexpr char kIndexSalt[] = "__pdx5_index__";
constexpr std::uint32_t kStreamingChunkSize = 256u * 1024u;
constexpr std::size_t kAeadRecordOverhead = 12u + 16u;
constexpr std::uint64_t kMaxEntrySize = 8ull * 1024ull * 1024ull * 1024ull;
constexpr std::size_t kMaxInputs = 1'000'000;

struct ScopedDirectory {
    std::filesystem::path path;
    bool keep = false;

    ~ScopedDirectory() {
        if (keep || path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct ValidatedInput {
    PackageInput input;
    std::filesystem::path source;
};

struct ArchiveEntry {
    std::string uri;
    std::filesystem::path source;
    Bytes data;
    std::uint64_t size = 0;
    std::string fingerprint;

    [[nodiscard]] bool FileBacked() const { return !source.empty(); }
};

struct CachedArchiveEntry {
    std::string uri;
    std::string fingerprint;
    std::uint64_t offset = 0;
    std::uint64_t rawSize = 0;
    std::uint64_t storedSize = 0;
    std::uint32_t crc = 0;
};

struct ArchiveCache {
    std::filesystem::path archivePath;
    std::unordered_map<std::string, CachedArchiveEntry> entries;
};

struct ValidationResult {
    std::vector<ValidatedInput> inputs;
    std::vector<PackageDiagnostic> diagnostics;
    std::uint64_t deferredMediaCount = 0;
    bool cancelled = false;
};

std::filesystem::path Utf8Path(const std::string_view value) { return px::PathFromUtf8(value); }

void AddDiagnostic(std::vector<PackageDiagnostic>& diagnostics, std::string code,
                   std::string message, const bool retryable = false,
                   std::string path = {}) {
    PackageDiagnostic diagnostic{std::move(code), std::move(message), retryable};
    if (!path.empty()) {
        diagnostic.span = PackageDiagnostic::SourceSpan{.path = std::move(path)};
    }
    diagnostics.push_back(std::move(diagnostic));
}

std::filesystem::path JsonPath(const Json& root, const char* key) {
    const auto found = root.find(key);
    if (found == root.end() || !found->is_string()) return {};
    return Utf8Path(found->get<std::string>());
}

bool RequiredString(const Json& root, const char* key, std::string& value, std::vector<PackageDiagnostic>& diagnostics) {
    const auto found = root.find(key);
    if (found == root.end() || !found->is_string() || found->empty()) {
        AddDiagnostic(diagnostics, "PXPKG1101", std::string(key) + " must be a non-empty string");
        return false;
    }
    value = found->get<std::string>();
    return true;
}

std::optional<int> JsonInt(const Json& value) {
    if (!value.is_number_integer()) return std::nullopt;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
            return std::nullopt;
        }
        return static_cast<int>(number);
    }
    const auto number = value.get<std::int64_t>();
    if (number < (std::numeric_limits<int>::min)() || number > (std::numeric_limits<int>::max)()) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

bool IsRequestId(const std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](const unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.'; });
}

bool IsLocaleId(const std::string_view value) {
    std::size_t offset = 0;
    const auto separator = value.find('-');
    const auto language = value.substr(0, separator);
    if (language.size() < 2 || language.size() > 3 ||
        !std::ranges::all_of(language, [](const unsigned char character) {
            return std::isalpha(character) != 0;
        }))
        return false;
    if (separator == std::string_view::npos) return true;
    offset = separator + 1;
    while (offset < value.size()) {
        const auto next = value.find('-', offset);
        const auto segment = value.substr(
            offset, next == std::string_view::npos ? value.size() - offset
                                                    : next - offset);
        if (segment.size() < 2 || segment.size() > 8 ||
            !std::ranges::all_of(segment, [](const unsigned char character) {
                return std::isalnum(character) != 0;
            }))
            return false;
        if (next == std::string_view::npos) return true;
        offset = next + 1;
    }
    return false;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsStreamingMediaPath(const std::string_view uri) {
    const std::string extension =
        Lower(px::PathToUtf8(Utf8Path(uri).extension()));
    constexpr std::array<std::string_view, 9> extensions{
        ".mp4", ".m4v", ".mov", ".mkv", ".webm", ".avi", ".ogv", ".ts", ".wmv"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

bool IsAlreadyCompressedMediaPath(const std::string_view uri) {
    const std::string extension =
        Lower(px::PathToUtf8(Utf8Path(uri).extension()));
    constexpr std::array<std::string_view, 13> extensions{
        ".png", ".jpg", ".jpeg", ".webp", ".avif", ".gif",
        ".ogg", ".opus", ".mp3", ".flac", ".m4a", ".aac", ".wma"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

// These assets are not parsed by the project/Story/UI contract checks. Every
// archive path below independently hashes the source before promotion: the
// stream writer hashes the bytes it writes, cached entries recheck the source,
// and whole-shard reuse rehashes current sources. Deferring their first hash
// avoids reading large media twice while preserving a byte-level check.
bool CanDeferInputFingerprint(const std::string_view uri) {
    // MPEG transport streams and TypeScript modules share the .ts extension.
    // An authored module may be read during structural validation, so keep
    // ambiguous .ts inputs on the eager path.
    if (Lower(px::PathToUtf8(Utf8Path(uri).extension())) == ".ts")
        return false;
    return IsAlreadyCompressedMediaPath(uri) || IsStreamingMediaPath(uri);
}

std::string ChunkContext(const std::string_view name,
                         const std::uint32_t index,
                         const std::uint32_t rawBytes) {
    return std::string(name) + "#pdx5:" + std::to_string(index) + ":" +
           std::to_string(rawBytes);
}

bool IsFingerprint(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const unsigned char c) { return std::isxdigit(c) != 0; });
}

bool IsSafeUri(const std::string_view uri) {
    if (uri.empty() || uri.size() > 4096 || uri.front() == '/' || uri.find('\\') != std::string_view::npos || uri.find(':') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path = Utf8Path(uri);
    if (path.is_absolute() || px::PathToUtf8(path) != uri) return false;
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") return false;
    }
    return px::PathToUtf8(path.lexically_normal()) == uri;
}

std::filesystem::path WeakCanonical(const std::filesystem::path& path, std::error_code& error) {
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) return {};
    return result.lexically_normal();
}

bool EqualComponent(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    return Lower(px::PathToUtf8(left)) == Lower(px::PathToUtf8(right));
#else
    return left == right;
#endif
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto childIt = child.begin();
    for (auto parentIt = parent.begin(); parentIt != parent.end(); ++parentIt) {
        if (childIt == child.end() || !EqualComponent(*childIt, *parentIt)) {
            return false;
        }
        ++childIt;
    }
    return true;
}

bool CryptoReady() {
    static const bool ready = psa_crypto_init() == PSA_SUCCESS;
    return ready;
}

bool ValidPublisherPublicKey(const std::string_view hex) {
    if (hex.size() != 130 || !hex.starts_with("04") || !CryptoReady())
        return false;
    std::array<std::uint8_t, 65> encoded{};
    const auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto high = nibble(hex[index * 2]);
        const auto low = nibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        encoded[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(
                                      PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    const auto imported = psa_import_key(&attributes, encoded.data(),
                                         encoded.size(), &keyId);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return false;
    return psa_destroy_key(keyId) == PSA_SUCCESS;
}

std::optional<Key> Sha256Bytes(const std::uint8_t* data, const std::size_t size) {
    if (!CryptoReady()) return std::nullopt;
    Key digest{};
    std::size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, size, digest.data(), digest.size(), &written) != PSA_SUCCESS || written != digest.size()) {
        return std::nullopt;
    }
    return digest;
}

std::optional<Key> Sha256Text(const std::string_view text) { return Sha256Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }

std::string Hex(const Key& digest) {
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const auto byte : digest) text << std::setw(2) << static_cast<int>(byte);
    return text.str();
}

Key DeriveKey(const std::string_view passphrase) { return Sha256Text(passphrase).value_or(Key{}); }

Iv DeriveIv(const std::string_view salt) {
    const auto digest = Sha256Text(salt).value_or(Key{});
    Iv result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(digest[index] ^ digest[index + result.size()]);
    }
    return result;
}

std::optional<Bytes> Encrypt(const Bytes& input, const Key& key, const Iv& iv,
                             const std::string_view context = {}) {
    constexpr std::size_t nonceSize = 12;
    constexpr std::size_t tagSize = 16;
    Bytes seed(iv.begin(), iv.end());
    seed.insert(seed.end(), input.begin(), input.end());
    const auto digest = Sha256Bytes(seed.data(), seed.size());
    if (!digest || !CryptoReady()) return std::nullopt;

    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key.size() * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    const psa_status_t imported = psa_import_key(&attributes, key.data(), key.size(), &keyId);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return std::nullopt;

    Bytes output(nonceSize + input.size() + tagSize);
    std::copy_n(digest->begin(), nonceSize, output.begin());
    std::size_t written = 0;
    const psa_status_t encrypted = psa_aead_encrypt(keyId, PSA_ALG_GCM, output.data(), nonceSize, reinterpret_cast<const std::uint8_t*>(context.data()), context.size(), input.data(), input.size(), output.data() + nonceSize, output.size() - nonceSize, &written);
    psa_destroy_key(keyId);
    if (encrypted != PSA_SUCCESS) return std::nullopt;
    output.resize(nonceSize + written);
    return output;
}

std::uint64_t HashPath(const std::string_view path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : path) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void UpdateCrc32(std::uint32_t& crc, const std::uint8_t* data,
                 const std::size_t size) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1u) ^ (0xedb88320u & (0u - (value & 1u)));
            values[index] = value;
        }
        return values;
    }();
    for (std::size_t index = 0; index < size; ++index)
        crc = (crc >> 8u) ^ table[(crc ^ data[index]) & 0xffu];
}

std::uint32_t Crc32(const Bytes& data) {
    std::uint32_t crc = 0xffffffffu;
    UpdateCrc32(crc, data.data(), data.size());
    return ~crc;
}

void PutU16(Bytes& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void PutU32(Bytes& bytes, const std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void PutU64(Bytes& bytes, const std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

int CompressionLevel(const PackageCompression compression) {
    switch (compression) {
        case PackageCompression::None:
            return 0;
        case PackageCompression::Fast:
            return 1;
        case PackageCompression::Balanced:
            return 9;
        case PackageCompression::Maximum:
            return 19;
    }
    return 0;
}

bool CouldBeJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return true;  // The subsequent read reports the actual failure.
    std::array<unsigned char, 4096> prefix{};
    input.read(reinterpret_cast<char*>(prefix.data()),
               static_cast<std::streamsize>(prefix.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    std::size_t offset = 0;
    if (count >= 3 && prefix[0] == 0xef && prefix[1] == 0xbb &&
        prefix[2] == 0xbf) offset = 3;
    for (; offset < count; ++offset) {
        const unsigned char value = prefix[offset];
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n')
            continue;
        return value == '{' || value == '[' || value == '"' || value == '-' ||
               (value >= '0' && value <= '9') || value == 't' || value == 'f' ||
               value == 'n';
    }
    // A long whitespace prefix may still precede a valid JSON value.
    return !input.eof();
}

enum class ArchiveWriteResult {
    Completed,
    InputChanged,
    Failed,
};

struct StreamDigest {
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint64_t size = 0;
    bool valid = false;

    StreamDigest() {
        valid = CryptoReady() &&
                psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    }
    StreamDigest(const StreamDigest&) = delete;
    StreamDigest& operator=(const StreamDigest&) = delete;
    ~StreamDigest() {
        if (valid) psa_hash_abort(&hash);
    }

    bool Update(const std::uint8_t* bytes, const std::size_t count) {
        if (!valid || size > (std::numeric_limits<std::uint64_t>::max)() - count ||
            psa_hash_update(&hash, bytes, count) != PSA_SUCCESS)
            return false;
        UpdateCrc32(crc, bytes, count);
        size += count;
        return true;
    }

    std::optional<std::string> Finish() {
        if (!valid) return std::nullopt;
        Key digest{};
        std::size_t written = 0;
        if (psa_hash_finish(&hash, digest.data(), digest.size(), &written) !=
                PSA_SUCCESS ||
            written != digest.size()) {
            psa_hash_abort(&hash);
            valid = false;
            return std::nullopt;
        }
        valid = false;
        return Hex(digest);
    }
};

bool CopyStream(std::istream& input, std::ostream& output,
                StreamDigest* digest = nullptr) {
    std::array<std::uint8_t, kStreamingChunkSize> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto size = static_cast<std::size_t>(count);
            if (digest && !digest->Update(buffer.data(), size)) return false;
            output.write(reinterpret_cast<const char*>(buffer.data()), count);
            if (!output) return false;
        }
    }
    return input.eof();
}

bool EncryptStreamChunked(std::istream& input, std::ostream& output,
                          const Key& key, const std::string_view uri,
                          std::uint32_t& chunkCount,
                          std::uint64_t& storedSize,
                          StreamDigest* digest = nullptr) {
    std::array<std::uint8_t, kStreamingChunkSize> buffer{};
    chunkCount = 0;
    storedSize = 0;
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        const auto size = static_cast<std::size_t>(count);
        if (digest && !digest->Update(buffer.data(), size)) return false;
        if (chunkCount == (std::numeric_limits<std::uint32_t>::max)())
            return false;
        Bytes chunk(buffer.begin(), buffer.begin() + count);
        const std::string context =
            ChunkContext(uri, chunkCount, static_cast<std::uint32_t>(size));
        const auto encrypted = Encrypt(chunk, key, DeriveIv(context), context);
        if (!encrypted || encrypted->size() != size + kAeadRecordOverhead)
            return false;
        output.write(reinterpret_cast<const char*>(encrypted->data()),
                     static_cast<std::streamsize>(encrypted->size()));
        if (!output || storedSize >
                           (std::numeric_limits<std::uint64_t>::max)() -
                               encrypted->size())
            return false;
        storedSize += encrypted->size();
        ++chunkCount;
    }
    return input.eof();
}

bool CompressFile(const ArchiveEntry& entry,
                  const std::filesystem::path& destination, const int level,
                  StreamDigest& digest, std::uint64_t& compressedSize) {
    std::ifstream input(entry.source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) return false;
    const auto context = std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>(
        ZSTD_createCCtx(), &ZSTD_freeCCtx);
    if (!context || ZSTD_isError(ZSTD_CCtx_setParameter(
                        context.get(), ZSTD_c_compressionLevel, level)) != 0)
        return false;
    std::array<std::uint8_t, kStreamingChunkSize> source{};
    std::array<std::uint8_t, kStreamingChunkSize> target{};
    bool finished = false;
    while (!finished) {
        input.read(reinterpret_cast<char*>(source.data()),
                   static_cast<std::streamsize>(source.size()));
        const auto count = input.gcount();
        if (count > 0 &&
            !digest.Update(source.data(), static_cast<std::size_t>(count)))
            return false;
        if (!input && !input.eof()) return false;
        ZSTD_inBuffer in{source.data(), static_cast<std::size_t>(count), 0};
        const ZSTD_EndDirective directive = input.eof() ? ZSTD_e_end : ZSTD_e_continue;
        do {
            ZSTD_outBuffer out{target.data(), target.size(), 0};
            const std::size_t remaining =
                ZSTD_compressStream2(context.get(), &out, &in, directive);
            if (ZSTD_isError(remaining) != 0) return false;
            output.write(reinterpret_cast<const char*>(target.data()),
                         static_cast<std::streamsize>(out.pos));
            if (!output) return false;
            compressedSize += out.pos;
            finished = directive == ZSTD_e_end && remaining == 0;
        } while (in.pos < in.size || (directive == ZSTD_e_end && !finished));
    }
    return true;
}

std::string ShardArchiveName(const std::uint32_t index,
                             const std::uint32_t count) {
    if (count == 1) return "Content.pdx";
    return "Content-" + (index < 10 ? std::string("0") : std::string{}) +
           std::to_string(index) + ".pdx";
}

std::string ShardCacheName(const std::uint32_t index,
                           const std::uint32_t count) {
    if (count == 1) return "entry-cache.json";
    return "entry-cache-" + (index < 10 ? std::string("0") : std::string{}) +
           std::to_string(index) + ".json";
}

std::uint32_t ArchiveShardForUri(const std::string_view uri,
                                  const std::uint32_t count) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char value : uri) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return static_cast<std::uint32_t>(hash % count);
}

struct ShardReuseRecord {
    std::string contentFingerprint;
    std::string archiveFingerprint;
};

using ShardReuseIndex = std::unordered_map<std::string, ShardReuseRecord>;

std::optional<std::string> ShardContentFingerprint(
    const std::string_view archiveName,
    const std::vector<ArchiveEntry>& entries,
    const PackageCompression compression,
    const bool encryption,
    const std::string_view encryptionKey) {
    std::string material = "PrismatiXArchiveShard|1|PDX5|";
    material += std::string(archiveName) + "|" +
                std::to_string(CompressionLevel(compression)) + "|";
    material += encryption ? std::string(encryptionKey) : "plaintext";
    for (const auto& entry : entries) {
        if (!IsFingerprint(entry.fingerprint)) return std::nullopt;
        material += "|" + std::to_string(entry.uri.size()) + ":" + entry.uri +
                    "|" + std::to_string(entry.size) + "|" + entry.fingerprint;
    }
    const auto digest = Sha256Text(material);
    return digest ? std::optional<std::string>(Hex(*digest)) : std::nullopt;
}

std::optional<ShardReuseIndex> LoadShardReuseIndex(
    const std::filesystem::path& previousOutput,
    const std::uint32_t shardCount,
    const PackageCompression compression,
    const bool encryption) {
    const auto indexPath = previousOutput / "Package/archive-shards.json";
    std::error_code error;
    const auto size = std::filesystem::file_size(indexPath, error);
    if (error || size == 0 || size > 64ull * 1024ull) return std::nullopt;
    std::ifstream input(indexPath, std::ios::binary);
    if (!input) return std::nullopt;
    std::string source(static_cast<std::size_t>(size), '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (!input) return std::nullopt;
    const Json root = Json::parse(source, nullptr, false);
    if (!root.is_object() || !root.contains("format") ||
        !root["format"].is_string() ||
        root["format"] != "PrismatiXArchiveShardIndex" ||
        !root.contains("schemaRevision") ||
        JsonInt(root["schemaRevision"]) != 1 ||
        !root.contains("archiveShards") ||
        JsonInt(root["archiveShards"]) != static_cast<int>(shardCount) ||
        !root.contains("compressionLevel") ||
        JsonInt(root["compressionLevel"]) != CompressionLevel(compression) ||
        !root.contains("encrypted") || !root["encrypted"].is_boolean() ||
        root["encrypted"].get<bool>() != encryption ||
        !root.contains("archives") || !root["archives"].is_array() ||
        root["archives"].size() > shardCount)
        return std::nullopt;
    ShardReuseIndex result;
    for (const auto& item : root["archives"]) {
        if (!item.is_object() || !item.contains("file") ||
            !item["file"].is_string() ||
            !item.contains("contentFingerprint") ||
            !item["contentFingerprint"].is_string() ||
            !item.contains("archiveFingerprint") ||
            !item["archiveFingerprint"].is_string())
            return std::nullopt;
        const std::string file = item["file"].get<std::string>();
        bool expectedName = false;
        for (std::uint32_t index = 0; index < shardCount; ++index)
            expectedName |= file == ShardArchiveName(index, shardCount);
        ShardReuseRecord record{
            item["contentFingerprint"].get<std::string>(),
            item["archiveFingerprint"].get<std::string>()};
        if (!expectedName || !IsFingerprint(record.contentFingerprint) ||
            !IsFingerprint(record.archiveFingerprint) ||
            !result.emplace(file, std::move(record)).second)
            return std::nullopt;
    }
    return result;
}

std::optional<ArchiveCache> LoadArchiveCache(
    const std::filesystem::path& previousOutput, const int compressionLevel,
    const std::string_view archiveName, const std::string_view cacheName) {
    const auto cachePath = previousOutput / "Package" / cacheName;
    const auto archivePath = previousOutput / archiveName;
    std::error_code error;
    const auto cacheSize = std::filesystem::file_size(cachePath, error);
    if (error || cacheSize == 0 || cacheSize > 64ull * 1024ull * 1024ull)
        return std::nullopt;
    error.clear();
    const auto archiveSize = std::filesystem::file_size(archivePath, error);
    if (error || archiveSize < kArchiveHeaderSize) return std::nullopt;
    std::ifstream input(cachePath, std::ios::binary);
    if (!input) return std::nullopt;
    std::string source(static_cast<std::size_t>(cacheSize), '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (!input) return std::nullopt;
    const Json root = Json::parse(source, nullptr, false);
    if (!root.is_object() || !root.contains("format") ||
        !root["format"].is_string() ||
        root["format"].get<std::string>() != "PrismatiXPackageEntryCache" ||
        !root.contains("schemaRevision") ||
        !root["schemaRevision"].is_number_integer() ||
        root["schemaRevision"].get<int>() != 1 ||
        !root.contains("compressionLevel") ||
        !root["compressionLevel"].is_number_integer() ||
        root["compressionLevel"].get<std::int64_t>() < 0 ||
        root["compressionLevel"].get<std::int64_t>() > 19 ||
        root["compressionLevel"].get<int>() != compressionLevel ||
        !root.contains("entries") || !root["entries"].is_array() ||
        root["entries"].size() > kMaxInputs) return std::nullopt;
    ArchiveCache cache{.archivePath = archivePath};
    for (const auto& item : root["entries"]) {
        if (!item.is_object() || !item.contains("uri") ||
            !item["uri"].is_string() || !item.contains("fingerprint") ||
            !item["fingerprint"].is_string() || !item.contains("offset") ||
            !item["offset"].is_number_unsigned() ||
            !item.contains("rawSize") || !item["rawSize"].is_number_unsigned() ||
            !item.contains("storedSize") || !item["storedSize"].is_number_unsigned() ||
            !item.contains("crc") || !item["crc"].is_number_unsigned() ||
            item["crc"].get<std::uint64_t>() >
                (std::numeric_limits<std::uint32_t>::max)())
            return std::nullopt;
        CachedArchiveEntry entry{
            .uri = item["uri"].get<std::string>(),
            .fingerprint = item["fingerprint"].get<std::string>(),
            .offset = item["offset"].get<std::uint64_t>(),
            .rawSize = item["rawSize"].get<std::uint64_t>(),
            .storedSize = item["storedSize"].get<std::uint64_t>(),
            .crc = item["crc"].get<std::uint32_t>()};
        if (!IsSafeUri(entry.uri) || !IsFingerprint(entry.fingerprint) ||
            entry.offset < kArchiveHeaderSize || entry.offset > archiveSize ||
            entry.storedSize == 0 || entry.storedSize >= entry.rawSize ||
            entry.storedSize > archiveSize - entry.offset)
            return std::nullopt;
        const std::string key = entry.uri;
        if (!cache.entries.emplace(key, std::move(entry)).second)
            return std::nullopt;
    }
    return cache;
}

bool VerifyOrCopyCachedEntry(const ArchiveCache& cache,
                             const CachedArchiveEntry& entry,
                             std::ostream* output) {
    std::ifstream input(cache.archivePath, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(entry.offset));
    const auto context = std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>(
        ZSTD_createDCtx(), &ZSTD_freeDCtx);
    StreamDigest digest;
    if (!input || !context || !digest.valid) return false;
    std::array<std::uint8_t, kStreamingChunkSize> source{};
    std::array<std::uint8_t, kStreamingChunkSize> decoded{};
    std::uint64_t remaining = entry.storedSize;
    bool finished = false;
    while (remaining > 0) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, source.size()));
        input.read(reinterpret_cast<char*>(source.data()),
                   static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count)) return false;
        if (output) {
            output->write(reinterpret_cast<const char*>(source.data()),
                          static_cast<std::streamsize>(count));
            if (!*output) return false;
        }
        remaining -= count;
        ZSTD_inBuffer compressed{source.data(), count, 0};
        while (compressed.pos < compressed.size) {
            const auto before = compressed.pos;
            ZSTD_outBuffer plain{decoded.data(), decoded.size(), 0};
            const auto needed = ZSTD_decompressStream(
                context.get(), &plain, &compressed);
            if (ZSTD_isError(needed) != 0 ||
                digest.size > entry.rawSize ||
                plain.pos > entry.rawSize - digest.size ||
                !digest.Update(decoded.data(), plain.pos)) return false;
            if (needed == 0) {
                finished = true;
                if (compressed.pos != compressed.size || remaining != 0)
                    return false;
            } else if (plain.pos == 0 && compressed.pos == before) {
                return false;
            }
        }
    }
    const auto fingerprint = digest.Finish();
    return finished && fingerprint && digest.size == entry.rawSize &&
           *fingerprint == Lower(entry.fingerprint) && ~digest.crc == entry.crc;
}

ArchiveWriteResult WriteArchive(const std::filesystem::path& path,
                                const std::vector<ArchiveEntry>& entries,
                                const PackageCompression compression,
                                const bool encryption, const Key& key,
                                const ArchiveCache* cache,
                                std::vector<CachedArchiveEntry>& cacheEntries,
                                std::size_t& reusedEntries,
                                std::string& failure,
                                const std::function<void(std::size_t,
                                                         std::size_t,
                                                         const std::string&)>&
                                    progress) {
    const auto failed = [&](std::string message) {
        failure = std::move(message);
        return ArchiveWriteResult::Failed;
    };
    Bytes index;
    PutU32(index, static_cast<std::uint32_t>(entries.size()));
    std::unordered_set<std::uint64_t> hashes;
    const int compressionLevel = CompressionLevel(compression);
    cacheEntries.clear();
    reusedEntries = 0;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return failed("the output archive could not be opened");
    std::array<std::uint8_t, kArchiveHeaderSize> placeholder{};
    output.write(reinterpret_cast<const char*>(placeholder.data()),
                 static_cast<std::streamsize>(placeholder.size()));
    if (!output) return failed("the archive header placeholder could not be written");

    const auto workDirectory = path.parent_path() / ".pdx-work";
    std::error_code error;
    std::filesystem::create_directories(workDirectory, error);
    if (error) return failed("the compression work directory could not be created: " + error.message());
    ScopedDirectory cleanup{workDirectory};

    for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
        const auto& entry = entries[entryIndex];
        if (progress &&
            (entryIndex == 0 || (entryIndex + 1) % 256 == 0 ||
             entryIndex + 1 == entries.size())) {
            progress(entryIndex + 1, entries.size(),
                     "Writing " + entry.uri);
        }
        const std::uint64_t rawSize = entry.FileBacked() ? entry.size : entry.data.size();
        if (!IsSafeUri(entry.uri) || entry.uri.size() > static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()) || rawSize > kMaxEntrySize) {
            return failed("invalid archive entry: " + entry.uri);
        }
        const std::uint64_t pathHash = HashPath(entry.uri);
        if (!hashes.insert(pathHash).second)
            return failed("archive path hash collision: " + entry.uri);

        std::uint8_t flags = 0;
        std::uint32_t crc = 0;
        std::uint32_t chunkCount = 0;
        std::uint64_t storedSize = 0;
        const std::uint64_t offset = static_cast<std::uint64_t>(output.tellp());
        const auto temporary = workDirectory / (std::to_string(entryIndex) + ".zst");
        bool useCompressedFile = false;
        bool useCachedEntry = false;

        if (cache && !encryption && entry.FileBacked() &&
            compressionLevel != 0 && rawSize > 0 &&
            !IsStreamingMediaPath(entry.uri) &&
            !IsAlreadyCompressedMediaPath(entry.uri)) {
            const auto found = cache->entries.find(entry.uri);
            if (found != cache->entries.end()) {
                const auto& cached = found->second;
                if (cached.rawSize == rawSize &&
                    Lower(cached.fingerprint) == Lower(entry.fingerprint) &&
                    VerifyOrCopyCachedEntry(*cache, cached, nullptr)) {
                    // The cache is only a transformed-byte optimization. The
                    // current source still gets a writer-time TOCTOU check.
                    if (ComputePackageFingerprint(entry.source) !=
                        Lower(entry.fingerprint))
                        return ArchiveWriteResult::InputChanged;
                    if (!VerifyOrCopyCachedEntry(*cache, cached, &output))
                        return failed("cached archive entry changed while copying: " + entry.uri);
                    useCachedEntry = true;
                    flags = kEntryCompressed;
                    storedSize = cached.storedSize;
                    crc = cached.crc;
                    ++reusedEntries;
                }
            }
        }

        if (!useCachedEntry && !encryption && entry.FileBacked() && compressionLevel != 0 && rawSize > 0 &&
            !IsStreamingMediaPath(entry.uri) &&
            !IsAlreadyCompressedMediaPath(entry.uri)) {
            StreamDigest digest;
            std::uint64_t compressedSize = 0;
            if (!digest.valid ||
                !CompressFile(entry, temporary, compressionLevel, digest,
                              compressedSize))
                return failed("archive entry could not be compressed: " + entry.uri);
            const auto fingerprint = digest.Finish();
            if (!fingerprint || digest.size != entry.size ||
                *fingerprint != Lower(entry.fingerprint))
                return ArchiveWriteResult::InputChanged;
            crc = ~digest.crc;
            useCompressedFile = compressedSize < rawSize;
            if (useCompressedFile) flags |= kEntryCompressed;
        }

        auto writeInput = [&](std::istream& input, StreamDigest* digest) {
            if (encryption) {
                if (rawSize == 0) {
                    const auto encrypted =
                        Encrypt(Bytes{}, key, DeriveIv(entry.uri));
                    if (!encrypted) return false;
                    flags |= kEntryEncrypted;
                    output.write(
                        reinterpret_cast<const char*>(encrypted->data()),
                        static_cast<std::streamsize>(encrypted->size()));
                    storedSize = encrypted->size();
                    return static_cast<bool>(output);
                }
                flags |= kEntryEncrypted | kEntryChunkedEncrypted;
                return EncryptStreamChunked(input, output, key, entry.uri,
                                            chunkCount, storedSize, digest);
            }
            const auto before = static_cast<std::uint64_t>(output.tellp());
            if (!CopyStream(input, output, digest)) return false;
            storedSize = static_cast<std::uint64_t>(output.tellp()) - before;
            return true;
        };

        if (useCachedEntry) {
            // Verified transformed bytes have already been appended.
        } else if (useCompressedFile) {
            std::ifstream transformed(temporary, std::ios::binary);
            if (!transformed || !writeInput(transformed, nullptr))
                return failed("compressed archive entry could not be written: " + entry.uri);
        } else if (entry.FileBacked()) {
            StreamDigest digest;
            std::ifstream input(entry.source, std::ios::binary);
            if (!digest.valid || !input || !writeInput(input, &digest))
                return failed("archive entry could not be read or written: " + entry.uri);
            const auto fingerprint = digest.Finish();
            if (!fingerprint || digest.size != entry.size ||
                *fingerprint != Lower(entry.fingerprint))
                return ArchiveWriteResult::InputChanged;
            crc = ~digest.crc;
        } else {
            crc = Crc32(entry.data);
            std::string bytes(reinterpret_cast<const char*>(entry.data.data()),
                              entry.data.size());
            std::istringstream input(std::move(bytes), std::ios::binary);
            if (!writeInput(input, nullptr))
                return failed("generated archive entry could not be written: " + entry.uri);
        }

        // Each transformed file has already been appended to Content.pdx. Keeping
        // every .zst until the whole archive completes doubles peak disk usage on
        // large projects and makes otherwise valid packages fail late.
        if (std::filesystem::exists(temporary, error)) {
            error.clear();
            std::filesystem::remove(temporary, error);
            if (error)
                return failed("temporary archive entry could not be removed: " +
                              entry.uri + ": " + error.message());
        }

        PutU64(index, pathHash);
        PutU16(index, static_cast<std::uint16_t>(entry.uri.size()));
        index.insert(index.end(), entry.uri.begin(), entry.uri.end());
        PutU64(index, offset);
        PutU64(index, rawSize);
        PutU64(index, storedSize);
        PutU32(index, crc);
        index.push_back(flags);
        if ((flags & kEntryChunkedEncrypted) != 0u) {
            PutU32(index, kStreamingChunkSize);
            PutU32(index, chunkCount);
        }
        if (entry.FileBacked() && !encryption &&
            (flags & kEntryCompressed) != 0u) {
            cacheEntries.push_back(CachedArchiveEntry{
                .uri = entry.uri, .fingerprint = Lower(entry.fingerprint),
                .offset = offset, .rawSize = rawSize,
                .storedSize = storedSize, .crc = crc});
        }
    }

    if (encryption) {
        auto encrypted = Encrypt(index, key, DeriveIv(kIndexSalt));
        if (!encrypted) return failed("the archive index could not be encrypted");
        index = std::move(*encrypted);
    }
    if (index.empty()) return failed("the archive index is empty");

    const std::uint64_t indexOffset = static_cast<std::uint64_t>(output.tellp());
    output.write(reinterpret_cast<const char*>(index.data()),
                 static_cast<std::streamsize>(index.size()));
    if (!output) return failed("the archive index could not be written");
    Bytes header;
    header.insert(header.end(), kArchiveMagic, kArchiveMagic + 4);
    PutU32(header, kArchiveVersion);
    PutU32(header, encryption ? kArchiveEncrypted : 0u);
    PutU64(header, indexOffset);
    PutU64(header, index.size());
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    output.close();
    return output ? ArchiveWriteResult::Completed
                  : failed("the final archive header could not be written");
}

std::optional<Bytes> ReadFile(const std::filesystem::path& path, const std::uint64_t expectedSize) {
    if (expectedSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) || expectedSize > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    Bytes result(static_cast<std::size_t>(expectedSize));
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path, const std::uint64_t expectedSize) {
    auto bytes = ReadFile(path, expectedSize);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

std::optional<std::string> ReadVerifiedTextFile(
    const ValidatedInput& input,
    std::vector<PackageDiagnostic>& diagnostics) {
    const auto text = ReadTextFile(input.source, input.input.size);
    if (!text) return std::nullopt;
    const auto digest = Sha256Text(*text);
    if (!digest || Hex(*digest) != Lower(input.input.fingerprint)) {
        AddDiagnostic(diagnostics, "PXPKG1216",
                      "input fingerprint changed: " + input.input.uri, true);
        return std::nullopt;
    }
    return text;
}

struct PreviousPersistence {
    std::vector<detail::PackagePersistenceReader> readers;
    std::vector<std::string> compatibleSaveFingerprints;
    std::string packageFingerprint;
    std::string contentVersion;
    std::string manifestSha256;
    std::uint32_t saveVersion = 0;
    std::string error;
};

PreviousPersistence ParsePreviousPersistence(
    const std::string_view text, const std::string_view gameId) {
    PreviousPersistence result;
    if (text.empty() || text.size() > 4 * 1024 * 1024) {
        result.error = "The previous package manifest has an invalid size";
        return result;
    }
    const auto parsed = detail::ParsePackageManifest(text);
    if (!parsed.Valid()) {
        result.error =
            "The previous package manifest is unreadable; refusing to discard its persistence reader";
        return result;
    }
    const auto& manifest = parsed.manifest;
    if (manifest.gameId != gameId) return result;
    const auto digest = Sha256Text(text);
    if (!digest) {
        result.error = "The previous package manifest could not be hashed";
        return result;
    }
    result.manifestSha256 = Hex(*digest);
    result.packageFingerprint = manifest.packageFingerprint;
    result.contentVersion = manifest.contentVersion;
    result.saveVersion = manifest.saveVersion;
    if (manifest.persistenceKeyId.empty()) {
        if (manifest.encrypted && !manifest.archiveKey.empty()) {
            result.readers.push_back(
                {"legacy-archive-v1", manifest.archiveKey});
        } else {
            result.readers.push_back(
                {"legacy-fingerprint-v1", manifest.packageFingerprint});
        }
    } else {
        result.readers = manifest.legacyPersistenceReaders;
        result.compatibleSaveFingerprints =
            manifest.compatibleSaveFingerprints;
    }
    return result;
}

PreviousPersistence DiscoverPreviousPersistence(
    const std::filesystem::path& outputDir, const std::string_view gameId) {
    PreviousPersistence result;
    const auto path = outputDir / "Package/manifest.json";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return result;
    if (error || !std::filesystem::is_regular_file(path, error)) {
        result.error = "The previous package manifest could not be inspected";
        return result;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > 4 * 1024 * 1024) {
        result.error = "The previous package manifest has an invalid size";
        return result;
    }
    const auto text = ReadTextFile(path, size);
    if (!text) {
        result.error = "The previous package manifest could not be read";
        return result;
    }
    return ParsePreviousPersistence(*text, gameId);
}

bool DecodeUtf8(const std::string_view text, std::set<std::uint32_t>& output) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        std::uint32_t codepoint = 0;
        std::size_t count = 0;
        if (first <= 0x7Fu) {
            codepoint = first;
            count = 1;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            codepoint = first & 0x1Fu;
            count = 2;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            codepoint = first & 0x0Fu;
            count = 3;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            codepoint = first & 0x07u;
            count = 4;
        } else {
            return false;
        }
        if (offset + count > text.size()) return false;
        for (std::size_t index = 1; index < count; ++index) {
            const auto continuation =
                static_cast<std::uint8_t>(text[offset + index]);
            if ((continuation & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (continuation & 0x3Fu);
        }
        if ((count == 3 && codepoint < 0x800u) ||
            (count == 4 && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
            return false;
        const bool layoutControl = codepoint <= 0x20u ||
            (codepoint >= 0x7Fu && codepoint <= 0x9Fu) ||
            (codepoint >= 0x200Bu && codepoint <= 0x200Fu) ||
            (codepoint >= 0x202Au && codepoint <= 0x202Eu) ||
            (codepoint >= 0x2060u && codepoint <= 0x206Fu) ||
            (codepoint >= 0xFE00u && codepoint <= 0xFE0Fu) ||
            (codepoint >= 0xE0100u && codepoint <= 0xE01EFu) ||
            codepoint == 0xFEFFu;
        if (!layoutControl) output.insert(codepoint);
        offset += count;
    }
    return true;
}

std::string CodepointName(const std::uint32_t codepoint) {
    std::ostringstream output;
    output << "U+" << std::uppercase << std::hex << std::setfill('0')
           << std::setw(codepoint <= 0xFFFFu ? 4 : 6) << codepoint;
    return output.str();
}

void ValidateLocaleFontCoverage(
    const Json& localeDocument, const std::string_view locale,
    const RuntimeIrDocument* runtime,
    const std::unordered_map<std::string, const ValidatedInput*>& inputs,
    std::vector<PackageDiagnostic>& diagnostics) {
    const auto chain = localeDocument.find("fontChain");
    if (chain == localeDocument.end()) return;
    if (!chain->is_array() || chain->empty() || chain->size() > 16) {
        AddDiagnostic(diagnostics, "PXPKG1264",
                      "locale fontChain must contain 1 to 16 font assets: " +
                          std::string(locale));
        return;
    }

    std::set<std::string> fontPaths;
    std::vector<const ValidatedInput*> fontInputs;
    for (const auto& value : *chain) {
        if (!value.is_string()) {
            AddDiagnostic(diagnostics, "PXPKG1264",
                          "locale fontChain entries must be font asset paths: " +
                              std::string(locale));
            continue;
        }
        const std::string path = value.get<std::string>();
        const auto found = inputs.find(path);
        if (!IsSafeUri(path) ||
            (!path.ends_with(".ttf") && !path.ends_with(".otf")) ||
            !fontPaths.insert(path).second || found == inputs.end()) {
            AddDiagnostic(diagnostics, "PXPKG1265",
                          "locale font is unsafe, duplicated, unsupported, or missing from package inputs: " +
                              std::string(locale) + " -> " + path);
            continue;
        }
        fontInputs.push_back(found->second);
    }
    if (fontInputs.size() != chain->size()) return;

    std::set<std::uint32_t> codepoints;
    bool validText = true;
    for (auto entry = localeDocument["strings"].begin();
         entry != localeDocument["strings"].end(); ++entry) {
        if (!entry.value().is_string() ||
            !DecodeUtf8(entry.value().get_ref<const std::string&>(), codepoints)) {
            validText = false;
            AddDiagnostic(diagnostics, "PXPKG1266",
                          "locale strings must contain valid UTF-8 text values: " +
                              std::string(locale) + " -> " + entry.key());
        }
    }
    if (runtime) {
        for (const auto& operation : runtime->operations) {
            validText = DecodeUtf8(operation.text, codepoints) && validText;
            for (const auto& [name, value] : operation.arguments) {
                (void)name;
                validText = DecodeUtf8(value, codepoints) && validText;
            }
        }
    }
    if (!validText) return;

    static const bool ttfReady = TTF_Init();
    if (!ttfReady) {
        AddDiagnostic(diagnostics, "PXPKG1267",
                      "font coverage preflight could not initialize SDL_ttf", true);
        return;
    }
    struct FontCloser {
        void operator()(TTF_Font* font) const { TTF_CloseFont(font); }
    };
    std::vector<std::unique_ptr<TTF_Font, FontCloser>> fonts;
    for (const auto* input : fontInputs) {
        const auto encoded = input->source.generic_u8string();
        const std::string path(reinterpret_cast<const char*>(encoded.data()),
                               encoded.size());
        std::unique_ptr<TTF_Font, FontCloser> font(
            TTF_OpenFont(path.c_str(), 16.0f));
        if (!font) {
            AddDiagnostic(diagnostics, "PXPKG1267",
                          "locale font cannot be opened: " +
                              std::string(locale) + " -> " + input->input.uri);
            continue;
        }
        fonts.push_back(std::move(font));
    }
    if (fonts.size() != fontInputs.size()) return;

    std::vector<std::uint32_t> missing;
    for (const auto codepoint : codepoints) {
        if (!std::ranges::any_of(fonts, [codepoint](const auto& font) {
                return TTF_FontHasGlyph(font.get(), codepoint);
            }))
            missing.push_back(codepoint);
    }
    if (!missing.empty()) {
        std::ostringstream message;
        message << "locale fontChain does not cover " << missing.size()
                << " required glyph(s): " << locale << " -> ";
        const auto shown = (std::min)(missing.size(), std::size_t{16});
        for (std::size_t index = 0; index < shown; ++index) {
            if (index != 0) message << ", ";
            message << CodepointName(missing[index]);
        }
        if (shown != missing.size()) message << ", ...";
        AddDiagnostic(diagnostics, "PXPKG1268", message.str());
    }
}

bool IsUuid(const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) continue;
        if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> PlayerUiSceneId(const std::string_view text) {
    std::istringstream input{ std::string(text) };
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream header(line.substr(first));
        std::string kind;
        int version = 0;
        std::string id;
        std::string type;
        header >> kind >> version >> id >> type;
        if (kind == "@pxscene" && version == 4 && IsUuid(id) && type == "UIScene") {
            return id;
        }
        break;
    }
    const auto studio = ParseUi(text);
    return studio.Valid() && IsUuid(studio.document.id)
               ? std::optional<std::string>{studio.document.id}
               : std::nullopt;
}

std::string UuidFromFingerprint(const std::string_view fingerprint) {
    std::string uuid(fingerprint.substr(0, 32));
    if (uuid.size() < 32) uuid.append(32 - uuid.size(), '0');
    uuid[12] = '4';
    const unsigned int variant = static_cast<unsigned int>(std::isdigit(static_cast<unsigned char>(uuid[16])) ? uuid[16] - '0' : std::tolower(static_cast<unsigned char>(uuid[16])) - 'a' + 10);
    static constexpr char hex[] = "0123456789abcdef";
    uuid[16] = hex[(variant & 0x3u) | 0x8u];
    uuid.insert(8, "-");
    uuid.insert(13, "-");
    uuid.insert(18, "-");
    uuid.insert(23, "-");
    return uuid;
}

const ArchiveEntry* FindArchiveEntry(const std::vector<ArchiveEntry>& entries,
                                     const std::string_view uri) {
    const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
        return entry.uri == uri;
    });
    return found == entries.end() ? nullptr : &*found;
}

std::optional<Bytes> ReadArchiveEntry(const ArchiveEntry& entry,
                                      const std::uint64_t maximumSize =
                                          16ull * 1024ull * 1024ull) {
    if (!entry.FileBacked()) {
        return entry.data.size() <= maximumSize
                   ? std::optional<Bytes>(entry.data)
                   : std::nullopt;
    }
    if (entry.size > maximumSize) return std::nullopt;
    auto bytes = ReadFile(entry.source, entry.size);
    if (!bytes) return std::nullopt;
    const auto digest = Sha256Bytes(bytes->data(), bytes->size());
    if (!digest || Hex(*digest) != Lower(entry.fingerprint)) return std::nullopt;
    return bytes;
}

bool ShaderCrossReady() {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] { ready = SDL_ShaderCross_Init(); });
    return ready;
}

std::optional<std::array<float, 4>> EffectDefault(
    const Json& value, const std::string_view type) {
    std::array<float, 4> result{};
    const auto finite = [](const Json& item, float& output) {
        if (!item.is_number()) return false;
        const double number = item.get<double>();
        if (!std::isfinite(number) ||
            number < -(std::numeric_limits<float>::max)() ||
            number > (std::numeric_limits<float>::max)())
            return false;
        output = static_cast<float>(number);
        return true;
    };
    if (type == "number") {
        return finite(value, result[0])
                   ? std::optional<std::array<float, 4>>(result)
                   : std::nullopt;
    }
    const std::size_t required = type == "vec2" ? 2 : type == "color" ? 4 : 0;
    if (required == 0 || !value.is_array() || value.size() != required)
        return std::nullopt;
    for (std::size_t index = 0; index < required; ++index) {
        if (!finite(value[index], result[index])) return std::nullopt;
        if (type == "color" &&
            (result[index] < 0.0f || result[index] > 1.0f))
            return std::nullopt;
    }
    return result;
}

struct CompiledEffects {
    std::vector<detail::PackageCustomEffect> manifests;
    std::vector<ArchiveEntry> artifacts;
    std::set<std::string> sourceAssets;
    std::vector<PackageDiagnostic> diagnostics;
};

CompiledEffects CompileCustomEffects(const std::vector<ArchiveEntry>& entries) {
    CompiledEffects result;
    const ArchiveEntry* projectEntry = FindArchiveEntry(entries, "project.pxproject");
    if (!projectEntry) return result;
    const auto projectBytes = ReadArchiveEntry(*projectEntry);
    if (!projectBytes) {
        AddDiagnostic(result.diagnostics, "PXPKG1500",
                      "Project manifest is unavailable or over 16 MiB");
        return result;
    }
    const Json project = Json::parse(
        std::string(projectBytes->begin(), projectBytes->end()),
        nullptr, false);
    const auto effects = project.find("effects");
    if (effects == project.end()) return result;
    if (!effects->is_array() || effects->size() > 64) {
        AddDiagnostic(result.diagnostics, "PXPKG1500",
                      "Project effects must be a bounded array");
        return result;
    }
    if (!effects->empty() &&
        project.value("graphicsTier", std::string("basic")) != "gpu-effects") {
        AddDiagnostic(result.diagnostics, "PXPKG1501",
                      "Custom effects require graphicsTier gpu-effects");
        return result;
    }
    if (!effects->empty() && !ShaderCrossReady()) {
        AddDiagnostic(result.diagnostics, "PXPKG1502",
                      std::string("Offline shader compiler failed to initialize: ") +
                          SDL_GetError());
        return result;
    }

    std::set<std::string> ids;
    for (const Json& descriptor : *effects) {
        if (!descriptor.is_object() || descriptor.size() != 2 ||
            !descriptor.contains("id") || !descriptor["id"].is_string() ||
            !descriptor.contains("source") || !descriptor["source"].is_string()) {
            AddDiagnostic(result.diagnostics, "PXPKG1503",
                          "Custom effect descriptor is invalid");
            continue;
        }
        const std::string id = descriptor["id"].get<std::string>();
        const std::string manifestPath = descriptor["source"].get<std::string>();
        if (!IsRequestId(id) || !IsSafeUri(manifestPath) ||
            !manifestPath.ends_with(".pxeffect") || !ids.insert(id).second) {
            AddDiagnostic(result.diagnostics, "PXPKG1503",
                          "Custom effect identity/path is invalid or duplicated: " + id);
            continue;
        }
        const ArchiveEntry* manifestEntry = FindArchiveEntry(entries, manifestPath);
        const auto manifestBytes = manifestEntry
                                       ? ReadArchiveEntry(*manifestEntry)
                                       : std::optional<Bytes>{};
        const Json effect = manifestBytes
                                ? Json::parse(std::string(manifestBytes->begin(),
                                                         manifestBytes->end()),
                                              nullptr, false)
                                : Json(Json::value_t::discarded);
        const int schemaRevision =
            effect.is_object() ? effect.value("schemaRevision", 0) : 0;
        const std::string targetLayer =
            effect.is_object()
                ? effect.value("targetLayer", std::string{})
                : std::string{};
        if (!manifestEntry || effect.is_discarded() || !effect.is_object() ||
            effect.size() != 6 ||
            effect.value("format", std::string{}) != "PrismatiXEffect" ||
            (schemaRevision != 2 && schemaRevision != 3) ||
            effect.value("id", std::string{}) != id ||
            (targetLayer != "stage" && targetLayer != "node" &&
             targetLayer != "transition") ||
            (schemaRevision == 2 && targetLayer != "stage") ||
            !effect.contains("shader") || !effect["shader"].is_string() ||
            !effect.contains("uniforms") || !effect["uniforms"].is_array() ||
            effect["uniforms"].size() > 8) {
            AddDiagnostic(result.diagnostics, "PXPKG1504",
                          "Custom effect manifest is missing or invalid: " + manifestPath);
            continue;
        }
        const std::string shaderPath = effect["shader"].get<std::string>();
        const ArchiveEntry* shaderEntry = FindArchiveEntry(entries, shaderPath);
        const auto shaderBytes = shaderEntry
                                     ? ReadArchiveEntry(*shaderEntry, 1024 * 1024)
                                     : std::optional<Bytes>{};
        if (!IsSafeUri(shaderPath) || !shaderPath.ends_with(".hlsl") ||
            !shaderBytes || shaderBytes->empty()) {
            AddDiagnostic(result.diagnostics, "PXPKG1505",
                          "Custom effect HLSL is missing, unsafe, or over 1 MiB: " +
                              shaderPath);
            continue;
        }
        std::string hlsl(shaderBytes->begin(), shaderBytes->end());
        const std::uint32_t expectedSamplers =
            targetLayer == "transition" ? 2u : 1u;
        if (hlsl.find('\0') != std::string::npos ||
            hlsl.find("cbuffer PrismatiXEffectContext") == std::string::npos ||
            hlsl.find("register(b0, space3)") == std::string::npos ||
            hlsl.find("float4 parameters[8]") == std::string::npos ||
            (schemaRevision == 3 &&
             hlsl.find("float4 viewport") == std::string::npos) ||
            hlsl.find("register(t0, space2)") == std::string::npos ||
            hlsl.find("register(s0, space2)") == std::string::npos ||
            (expectedSamplers == 2u &&
             (hlsl.find("register(t1, space2)") == std::string::npos ||
              hlsl.find("register(s1, space2)") == std::string::npos))) {
            AddDiagnostic(result.diagnostics, "PXPKG1506",
                          "Custom effect must use the fixed PrismatiXEffectContext, texture, and sampler bindings: " +
                              shaderPath);
            continue;
        }

        detail::PackageCustomEffect compiled;
        compiled.id = id;
        compiled.schemaRevision = static_cast<std::uint32_t>(schemaRevision);
        compiled.targetLayer = targetLayer;
        std::set<std::string> uniformNames;
        std::set<std::uint32_t> uniformSlots;
        bool uniformsValid = true;
        for (const Json& value : effect["uniforms"]) {
            if (!value.is_object() || value.size() < 4 || value.size() > 6 ||
                !value.contains("name") || !value["name"].is_string() ||
                !value.contains("type") || !value["type"].is_string() ||
                !value.contains("slot") || !value["slot"].is_number_unsigned() ||
                !value.contains("default")) {
                uniformsValid = false;
                break;
            }
            detail::PackageEffectUniform uniform;
            uniform.name = value["name"].get<std::string>();
            uniform.type = value["type"].get<std::string>();
            uniform.slot = value["slot"].get<std::uint32_t>();
            const auto defaultValue = EffectDefault(value["default"], uniform.type);
            const double minimum = value.value("minimum", 0.0);
            const double maximum = value.value("maximum", 1.0);
            const double floatLimit =
                static_cast<double>((std::numeric_limits<float>::max)());
            const std::size_t components = uniform.type == "number" ? 1u
                                         : uniform.type == "vec2" ? 2u
                                         : uniform.type == "color" ? 4u : 0u;
            bool defaultInRange = defaultValue && components > 0;
            if (defaultInRange) {
                for (std::size_t component = 0; component < components;
                     ++component) {
                    const double number = (*defaultValue)[component];
                    defaultInRange = defaultInRange && number >= minimum &&
                                     number <= maximum;
                }
            }
            if (!IsRequestId(uniform.name) || uniform.slot > 7 || !defaultValue ||
                !std::isfinite(minimum) || !std::isfinite(maximum) ||
                std::abs(minimum) > floatLimit ||
                std::abs(maximum) > floatLimit || minimum > maximum ||
                !defaultInRange || !uniformNames.insert(uniform.name).second ||
                !uniformSlots.insert(uniform.slot).second) {
                uniformsValid = false;
                break;
            }
            uniform.defaultValue = *defaultValue;
            uniform.minimum = static_cast<float>(minimum);
            uniform.maximum = static_cast<float>(maximum);
            compiled.uniforms.push_back(std::move(uniform));
        }
        if (!uniformsValid) {
            AddDiagnostic(result.diagnostics, "PXPKG1507",
                          "Custom effect uniform schema is invalid: " + manifestPath);
            continue;
        }

        SDL_ShaderCross_HLSL_Info hlslInfo{};
        hlslInfo.source = hlsl.c_str();
        hlslInfo.entrypoint = "main";
        hlslInfo.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
        std::size_t spirvSize = 0;
        void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &spirvSize);
        if (!spirv || spirvSize == 0 || spirvSize > 4 * 1024 * 1024) {
            AddDiagnostic(result.diagnostics, "PXPKG1508",
                          "Custom effect did not compile to bounded SPIR-V: " +
                              shaderPath + " (" + SDL_GetError() + ")");
            if (spirv) SDL_free(spirv);
            continue;
        }
        SDL_ShaderCross_GraphicsShaderMetadata* metadata =
            SDL_ShaderCross_ReflectGraphicsSPIRV(
                static_cast<const Uint8*>(spirv), spirvSize, 0);
        if (!metadata ||
            metadata->resource_info.num_samplers != expectedSamplers ||
            metadata->resource_info.num_storage_textures != 0 ||
            metadata->resource_info.num_storage_buffers != 0 ||
            metadata->resource_info.num_uniform_buffers != 1 ||
            metadata->num_outputs != 1) {
            AddDiagnostic(result.diagnostics, "PXPKG1509",
                          "Custom effect reflection exceeds the fixed resource schema: " +
                              shaderPath);
            if (metadata) SDL_free(metadata);
            SDL_free(spirv);
            continue;
        }
        compiled.samplerCount = metadata->resource_info.num_samplers;
        compiled.uniformBufferCount = metadata->resource_info.num_uniform_buffers;
        SDL_free(metadata);

        SDL_ShaderCross_SPIRV_Info spirvInfo{};
        spirvInfo.bytecode = static_cast<const Uint8*>(spirv);
        spirvInfo.bytecode_size = spirvSize;
        spirvInfo.entrypoint = "main";
        spirvInfo.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
        void* msl = SDL_ShaderCross_TranspileMSLFromSPIRV(&spirvInfo);
        const std::size_t mslSize =
            msl ? std::strlen(static_cast<const char*>(msl)) + 1 : 0;
        if (!msl || mslSize <= 1 || mslSize > 4 * 1024 * 1024) {
            AddDiagnostic(result.diagnostics, "PXPKG1510",
                          "Custom effect did not compile to the release MSL format: " +
                              shaderPath + " (" + SDL_GetError() + ")");
            if (msl) SDL_free(msl);
            SDL_free(spirv);
            continue;
        }

        const auto appendArtifact = [&](const std::string& format,
                                        const std::string& extension,
                                        const void* bytes,
                                        const std::size_t size) {
            const std::string asset = "Shaders/" + id + "/fragment." + extension;
            Bytes data(static_cast<const std::uint8_t*>(bytes),
                       static_cast<const std::uint8_t*>(bytes) + size);
            const auto digest = Sha256Bytes(data.data(), data.size());
            compiled.artifacts.push_back(
                {format, asset, digest ? Hex(*digest) : std::string{}});
            result.artifacts.push_back(ArchiveEntry{
                .uri = asset, .data = std::move(data), .size = size});
        };
        appendArtifact("spirv", "spv", spirv, spirvSize);
#ifdef __APPLE__
        appendArtifact("msl", "msl", msl, mslSize);
#else
        std::size_t dxilSize = 0;
        void* dxil = SDL_ShaderCross_CompileDXILFromSPIRV(&spirvInfo, &dxilSize);
        if (!dxil || dxilSize == 0 || dxilSize > 4 * 1024 * 1024) {
            AddDiagnostic(result.diagnostics, "PXPKG1510",
                          "Custom effect did not compile to the release DXIL format: " +
                              shaderPath + " (" + SDL_GetError() + ")");
            if (dxil) SDL_free(dxil);
            SDL_free(msl);
            SDL_free(spirv);
            continue;
        }
        appendArtifact("dxil", "dxil", dxil, dxilSize);
        appendArtifact("msl", "msl", msl, mslSize);
        SDL_free(dxil);
#endif
        SDL_free(msl);
        SDL_free(spirv);
        result.sourceAssets.insert(shaderPath);
        result.manifests.push_back(std::move(compiled));
    }
    std::ranges::sort(result.manifests, {}, &detail::PackageCustomEffect::id);
    std::ranges::sort(result.artifacts, {}, &ArchiveEntry::uri);
    return result;
}

detail::PackageManifest BuildPackageManifest(const PackageRequest& request,
                                     const std::string& packageFingerprint,
                                     const std::string& encryptionKey,
                                     const std::vector<ArchiveEntry>& entries,
                                     std::vector<detail::PackageArchiveMount> archives,
                                     std::vector<detail::PackageCustomEffect> customEffects,
                                     std::vector<detail::PackagePersistenceReader>
                                         legacyPersistenceReaders,
                                     std::vector<std::string>
                                         compatibleSaveFingerprints) {
    auto routesInOrder = request.routes;
    std::sort(routesInOrder.begin(), routesInOrder.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    detail::PackageManifest manifest;
    manifest.engineVersion = "0.2.0";
    manifest.gameId = request.gameId;
    manifest.title = request.title;
    manifest.width = request.width;
    manifest.height = request.height;
    manifest.startRuntimeIr = request.startScript;
    manifest.sourceMap = request.sourceMap;
    manifest.startRoute = request.startRoute;
    manifest.contentVersion = request.contentVersion;
    manifest.saveVersion = request.saveVersion;
    manifest.graphicsTier = request.graphicsTier;
    manifest.saveMigrations = request.saveMigrations;
    manifest.extensions = request.extensions;
    manifest.customEffects = std::move(customEffects);
    manifest.packageFingerprint = packageFingerprint;
    manifest.persistenceDataId = request.gameId;
    manifest.persistenceKeyId = "game-data-v1";
    manifest.legacyPersistenceReaders =
        std::move(legacyPersistenceReaders);
    manifest.compatibleSaveFingerprints =
        std::move(compatibleSaveFingerprints);
    manifest.encrypted = request.encryption;
    manifest.archiveKey = request.encryption ? encryptionKey : std::string{};
    manifest.publisherPublicKey = request.publisherPublicKey;
    manifest.archives = std::move(archives);
    for (const auto& route : routesInOrder) {
        const auto archived = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.uri == route.scene; });
        const auto sceneBytes = archived == entries.end()
                                    ? std::optional<Bytes>{}
                                    : ReadArchiveEntry(*archived);
        const auto sceneText = sceneBytes
                                   ? std::optional<std::string>(std::string(
                                         sceneBytes->begin(), sceneBytes->end()))
                                   : std::optional<std::string>{};
        const auto sceneId = sceneText ? PlayerUiSceneId(*sceneText) : std::optional<std::string>{};
        manifest.routes.push_back(
            {route.id, sceneId.value_or(UuidFromFingerprint(packageFingerprint)),
             route.scene});
    }
    return manifest;
}

bool RuntimeLibrary(const std::filesystem::path& path) {
    const std::string filename = Lower(px::PathToUtf8(path.filename()));
    if (filename.find("shadercross") != std::string::npos ||
        filename.find("dxcompiler") != std::string::npos ||
        filename == "dxil.dll" ||
        filename.find("spirv-cross") != std::string::npos)
        return false;
    const std::string extension = Lower(px::PathToUtf8(path.extension()));
#ifdef _WIN32
    return extension == ".dll";
#elif defined(__APPLE__)
    return extension == ".dylib";
#else
    return extension == ".so" || px::PathToUtf8(path.filename()).find(".so.") != std::string::npos;
#endif
}

bool Cancelled(const PackageRequest& request) {
    std::error_code error;
    return std::filesystem::is_regular_file(request.cancelFile, error);
}

void Emit(const PackageEventSink& sink, PackageEvent event) {
    if (sink) sink(event);
}

void EmitProgress(const PackageRequest& request, const PackageEventSink& sink, std::string phase, const std::uint64_t current, const std::uint64_t total, std::string message) {
    PackageEvent event;
    event.kind = PackageEventKind::Progress;
    event.requestId = request.requestId;
    event.phase = std::move(phase);
    event.current = current;
    event.total = total;
    event.message = std::move(message);
    Emit(sink, std::move(event));
}

PackageRunResult Fail(const PackageRequest& request, const PackageEventSink& sink, std::string code, std::string message, const bool retryable = false) {
    PackageDiagnostic diagnostic{std::move(code), std::move(message), retryable};
    PackageEvent event;
    event.kind = PackageEventKind::Failed;
    event.requestId = request.requestId;
    event.code = diagnostic.code;
    event.message = diagnostic.message;
    event.retryable = diagnostic.retryable;
    event.diagnostics.push_back(diagnostic);
    Emit(sink, std::move(event));
    PackageRunResult result;
    result.exitCode = PackageExitCode::Failed;
    result.diagnostics.push_back(std::move(diagnostic));
    return result;
}

PackageRunResult Fail(const PackageRequest& request, const PackageEventSink& sink,
                      std::vector<PackageDiagnostic> diagnostics) {
    PackageRunResult result;
    result.exitCode = PackageExitCode::Failed;
    result.diagnostics = std::move(diagnostics);
    PackageEvent event;
    event.kind = PackageEventKind::Failed;
    event.requestId = request.requestId;
    event.diagnostics = result.diagnostics;
    if (!event.diagnostics.empty()) {
        event.code = event.diagnostics.front().code;
        event.message = event.diagnostics.front().message;
        event.retryable = event.diagnostics.front().retryable;
    }
    Emit(sink, std::move(event));
    return result;
}

PackageRunResult Cancel(const PackageRequest& request, const PackageEventSink& sink, std::string message) {
    PackageEvent event;
    event.kind = PackageEventKind::Cancelled;
    event.requestId = request.requestId;
    event.message = message;
    Emit(sink, std::move(event));
    PackageRunResult result;
    result.exitCode = PackageExitCode::Cancelled;
    return result;
}

ValidationResult Validate(const PackageRequest& request,
                          const PackageEventSink& eventSink,
                          const PackageValidationDepth depth =
                              PackageValidationDepth::Structural,
                          const bool deferFingerprintsUntilArchive = false) {
    ValidationResult result;
    auto& diagnostics = result.diagnostics;
    struct LocalizedOperationIdentity {
        std::string operationId;
        std::string sourceId;
        std::string kind;
    };
    struct LocalizedProgram {
        std::string locale;
        std::string documentId;
        std::vector<LocalizedOperationIdentity> operations;
    };
    std::vector<LocalizedProgram> localizedPrograms;
    std::optional<std::size_t> equivalentPrimaryLocale;
    if (!IsRequestId(request.requestId)) {
        AddDiagnostic(diagnostics, "PXPKG1201", "requestId contains unsupported characters");
    }
    if (!IsRequestId(request.gameId)) {
        AddDiagnostic(diagnostics, "PXPKG1226", "gameId is invalid");
    }
    if (!request.projectRoot.is_absolute() || !request.outputDir.is_absolute() || !request.playerExecutable.is_absolute() || !request.cancelFile.is_absolute()) {
        AddDiagnostic(
            diagnostics,
            "PXPKG1202",
            "projectRoot, outputDir, playerExecutable, and cancelFile "
            "must be absolute"
        );
    }
    if (!request.cacheSourceDir.empty() &&
        (!request.cacheSourceDir.is_absolute() ||
         request.cacheSourceDir == request.cacheSourceDir.root_path() ||
         request.cacheSourceDir != request.cacheSourceDir.lexically_normal())) {
        AddDiagnostic(diagnostics, "PXPKG1240",
                      "cacheSourceDir must be a normalized absolute directory path");
    }
    if (request.title.empty() || request.title.size() > 512) {
        AddDiagnostic(diagnostics, "PXPKG1203", "title is invalid");
    }
    if (request.width < 320 || request.width > 16384 || request.height < 180 || request.height > 16384) {
        AddDiagnostic(diagnostics, "PXPKG1204", "width and height are outside the supported range");
    }
    if (!IsSafeUri(request.startScript) || !request.startScript.ends_with(".pxir")) {
        AddDiagnostic(diagnostics, "PXPKG1205", "startScript must be a project-relative .pxir runtime URI");
    }
    if (!IsSafeUri(request.sourceMap) || !request.sourceMap.ends_with(".pxmap")) {
        AddDiagnostic(diagnostics, "PXPKG1237",
                      "sourceMap must be a project-relative .pxmap URI");
    }
    if (request.contentVersion.empty() || request.contentVersion.size() > 128) {
        AddDiagnostic(diagnostics, "PXPKG1206", "contentVersion is invalid");
    }
    if (request.saveVersion == 0 || request.saveVersion > 1'000'000) {
        AddDiagnostic(diagnostics, "PXPKG1227", "saveVersion is invalid");
    }
    if (request.inputs.empty() || request.inputs.size() > kMaxInputs) {
        AddDiagnostic(diagnostics, "PXPKG1207", "inputs count is invalid");
    }
    if (request.archiveShards < 1 || request.archiveShards > 64) {
        AddDiagnostic(diagnostics, "PXPKG1241",
                      "archiveShards must be an integer from 1 to 64");
    }
    if (!request.publisherPublicKey.empty() &&
        !ValidPublisherPublicKey(request.publisherPublicKey))
        AddDiagnostic(diagnostics, "PXPKG1243",
                      "publisherPublicKey must be a valid lowercase SEC1 P-256 point");

    std::error_code error;
    const auto projectRoot = WeakCanonical(request.projectRoot, error);
    if (error || !std::filesystem::is_directory(projectRoot, error)) {
        AddDiagnostic(diagnostics, "PXPKG1208", "projectRoot is not a readable directory", true);
    }
    error.clear();
    const auto player = WeakCanonical(request.playerExecutable, error);
    if (error || !std::filesystem::is_regular_file(player, error)) {
        AddDiagnostic(diagnostics, "PXPKG1209", "playerExecutable is not a readable file", true);
    }
    if (request.outputDir == request.outputDir.root_path() || request.outputDir.filename().empty() || request.outputDir != request.outputDir.lexically_normal()) {
        AddDiagnostic(diagnostics, "PXPKG1210", "outputDir cannot be a filesystem root");
    }
    error.clear();
    const auto outputDir = WeakCanonical(request.outputDir, error);
    if (error || IsWithin(projectRoot, outputDir) || IsWithin(player.parent_path(), outputDir)) {
        AddDiagnostic(diagnostics, "PXPKG1225", "outputDir cannot equal or contain projectRoot or the Player directory");
    }

    std::set<std::string> uris;
    std::set<std::string> uriKeys;
    result.inputs.reserve(request.inputs.size());
    for (std::size_t index = 0; index < request.inputs.size(); ++index) {
        if (index > 0 && index % 256 == 0) {
            EmitProgress(request, eventSink, "validate", index,
                         request.inputs.size(), "Checking package inputs");
        }
        if (Cancelled(request)) {
            result.cancelled = true;
            return result;
        }
        const auto& input = request.inputs[index];
        if (!IsSafeUri(input.uri)) {
            AddDiagnostic(diagnostics, "PXPKG1211", "input URI is not a normalized project-relative path: " + input.uri);
            continue;
        }
        const std::string uriKey =
#ifdef _WIN32
            Lower(input.uri);
#else
            input.uri;
#endif
        if (!uriKeys.insert(uriKey).second) {
            AddDiagnostic(diagnostics, "PXPKG1212", "input URI is duplicated: " + input.uri);
            continue;
        }
        uris.insert(input.uri);
        if (!IsFingerprint(input.fingerprint)) {
            AddDiagnostic(diagnostics, "PXPKG1213", "input fingerprint must be SHA-256: " + input.uri);
            continue;
        }
        error.clear();
        const auto source = WeakCanonical(projectRoot / Utf8Path(input.uri), error);
        if (error || !IsWithin(source, projectRoot) || !std::filesystem::is_regular_file(source, error)) {
            AddDiagnostic(diagnostics, "PXPKG1214", "input is missing or escapes projectRoot: " + input.uri, true);
            continue;
        }
        error.clear();
        const auto actualSize = std::filesystem::file_size(source, error);
        if (error || actualSize != input.size) {
            AddDiagnostic(diagnostics, "PXPKG1215", "input size changed: " + input.uri, true);
            continue;
        }
        const bool deferredMedia =
            depth == PackageValidationDepth::Structural &&
            CanDeferInputFingerprint(input.uri);
        if (deferredMedia) ++result.deferredMediaCount;
        // A publishing run checks parsed document bytes at each read and
        // checks every file again while writing or reusing the archive.
        // Keep .ts eager: it may be an authored module, not video media.
        const bool deferForWrite = deferFingerprintsUntilArchive &&
            Lower(px::PathToUtf8(Utf8Path(input.uri).extension())) != ".ts";
        if (!deferredMedia && !deferForWrite) {
            const std::string actualFingerprint = ComputePackageFingerprint(source);
            if (actualFingerprint.empty() || actualFingerprint != Lower(input.fingerprint)) {
                AddDiagnostic(diagnostics, "PXPKG1216", "input fingerprint changed: " + input.uri, true);
                continue;
            }
        }
        result.inputs.push_back({ input, source });
    }
    EmitProgress(request, eventSink, "validate", request.inputs.size(),
                 request.inputs.size(),
                 deferFingerprintsUntilArchive
                     ? "Checked package inputs; source bytes will be verified while reading or writing"
                     : "Checked package inputs; " +
                           std::to_string(result.deferredMediaCount) +
                           " media fingerprint(s) will be verified during archive writing or reuse");
    if (Cancelled(request)) {
        result.cancelled = true;
        return result;
    }
    EmitProgress(request, eventSink, "validate-structure", 0, 1,
                 "Validating project, Story, UI, and resource contracts");
    std::set<std::string> migrationIds;
    std::unordered_map<std::string, const PackageSaveMigration*>
        migrationBySource;
    const auto migrationKey = [](const std::string_view content,
                                 const std::uint32_t version) {
        return std::string(content) + "\n" + std::to_string(version);
    };
    for (const auto& migration : request.saveMigrations) {
        const bool identityValid =
            IsRequestId(migration.id) &&
            !migration.fromContentVersion.empty() &&
            migration.fromContentVersion.size() <= 128 &&
            !migration.toContentVersion.empty() &&
            migration.toContentVersion.size() <= 128 &&
            migration.fromSaveVersion > 0 &&
            migration.fromSaveVersion <= 1'000'000 &&
            migration.toSaveVersion > 0 &&
            migration.toSaveVersion <= 1'000'000 &&
            IsSafeUri(migration.asset) &&
            migration.asset.ends_with(".pxsave-migration") &&
            migrationIds.insert(migration.id).second;
        const std::string sourceKey =
            migrationKey(migration.fromContentVersion,
                         migration.fromSaveVersion);
        if (!identityValid ||
            !migrationBySource.emplace(sourceKey, &migration).second) {
            AddDiagnostic(diagnostics, "PXPKG1232",
                          "save migration identity, endpoints, or asset is invalid or ambiguous");
            continue;
        }
        if (!uris.contains(migration.asset)) {
            AddDiagnostic(diagnostics, "PXPKG1233",
                          "save migration asset must be present in inputs: " +
                              migration.asset);
            continue;
        }
        const auto input = std::find_if(
            result.inputs.begin(), result.inputs.end(),
            [&](const auto& value) {
                return value.input.uri == migration.asset;
            });
        const auto text = input == result.inputs.end()
                               ? std::optional<std::string>{}
                               : ReadVerifiedTextFile(*input, diagnostics);
        const Json document = text
                                  ? Json::parse(*text, nullptr, false)
                                  : Json(Json::value_t::discarded);
        if (!text || document.is_discarded() || !document.is_object() ||
            document.value("format", std::string{}) !=
                "PrismatiXSaveMigration" ||
            document.value("schemaRevision", 0) != 2 ||
            document.value("id", std::string{}) != migration.id ||
            !document.contains("from") || !document["from"].is_object() ||
            !document.contains("to") || !document["to"].is_object() ||
            !document.contains("anchor") ||
            !document["anchor"].is_object() ||
            !document.contains("operations") ||
            !document["operations"].is_array() ||
            document["operations"].size() > 100'000 ||
            document["from"].value("contentVersion", std::string{}) !=
                migration.fromContentVersion ||
            document["from"].value("saveVersion", std::uint32_t{0}) !=
                migration.fromSaveVersion ||
            document["to"].value("contentVersion", std::string{}) !=
                migration.toContentVersion ||
            document["to"].value("saveVersion", std::uint32_t{0}) !=
                migration.toSaveVersion) {
            AddDiagnostic(diagnostics, "PXPKG1234",
                          "save migration asset does not match its manifest descriptor: " +
                              migration.asset);
        }
    }
    const std::string targetMigrationKey =
        migrationKey(request.contentVersion, request.saveVersion);
    for (const auto& migration : request.saveMigrations) {
        std::unordered_set<std::string> visited;
        std::string current = migrationKey(migration.fromContentVersion,
                                           migration.fromSaveVersion);
        for (std::size_t step = 0; current != targetMigrationKey; ++step) {
            if (step >= 64 || !visited.insert(current).second) {
                AddDiagnostic(diagnostics, "PXPKG1235",
                              "save migration chain contains a cycle or exceeds 64 steps");
                break;
            }
            const auto next = migrationBySource.find(current);
            if (next == migrationBySource.end()) {
                AddDiagnostic(diagnostics, "PXPKG1236",
                              "save migration chain does not reach the package content/save version");
                break;
            }
            current = migrationKey(next->second->toContentVersion,
                                   next->second->toSaveVersion);
        }
    }
    const auto projectInput = std::find_if(
        result.inputs.begin(), result.inputs.end(), [](const auto& input) {
            return input.input.uri == "project.pxproject";
        });
    if (projectInput == result.inputs.end()) {
        AddDiagnostic(diagnostics, "PXPKG1226",
                      "project.pxproject must be present in inputs");
    } else {
        const auto projectText =
            ReadVerifiedTextFile(*projectInput, diagnostics);
        std::unordered_map<std::string, const ValidatedInput*> packagedInputs;
        for (const auto& input : result.inputs)
            packagedInputs.emplace(input.input.uri, &input);
        const auto readPackaged =
            [&packagedInputs, &diagnostics](const std::string_view uri)
                -> std::optional<std::string> {
            const auto found = packagedInputs.find(std::string(uri));
            if (found == packagedInputs.end()) return std::nullopt;
            return ReadVerifiedTextFile(*found->second, diagnostics);
        };
        const auto packagedExists =
            [&packagedInputs](const std::string_view uri) {
                return packagedInputs.contains(std::string(uri));
            };
        if (!projectText) {
            AddDiagnostic(diagnostics, "PXPKG1227",
                          "project.pxproject could not be read", true);
        } else {
            const Json canonicalProject =
                Json::parse(*projectText, nullptr, false);
            bool projectValid = canonicalProject.is_object();
            static const std::set<std::string> projectFields{
                "format", "schemaRevision", "id", "name", "version",
                "contentVersion", "saveVersion", "graphicsTier",
                "effects",
                "saveMigrations", "engineCompatibility", "resolution",
                "entry", "defaultLocale", "supportedLocales", "storyIndex",
                "gameCatalog", "extensions", "uiEntryPoints", "assets",
                "characters", "uiComponents", "settings"};
            if (projectValid) {
                for (auto item = canonicalProject.begin();
                     item != canonicalProject.end(); ++item) {
                    if (!projectFields.contains(item.key())) projectValid = false;
                }
            }
            const auto requiredObject = [&canonicalProject](const char* name) {
                return canonicalProject.contains(name) &&
                       canonicalProject[name].is_object();
            };
            const auto requiredArray = [&canonicalProject](const char* name) {
                return canonicalProject.contains(name) &&
                       canonicalProject[name].is_array();
            };
            projectValid = projectValid &&
                canonicalProject.value("format", std::string{}) ==
                    "PrismatiXProject" &&
                canonicalProject.value("schemaRevision", 0) == 2 &&
                canonicalProject.value("id", std::string{}) == request.gameId &&
                !canonicalProject.value("name", std::string{}).empty() &&
                !canonicalProject.value("version", std::string{}).empty() &&
                canonicalProject.value("contentVersion", std::string{}) ==
                    request.contentVersion &&
                canonicalProject.value("saveVersion", std::uint32_t{0}) ==
                    request.saveVersion &&
                canonicalProject.value("graphicsTier", std::string("basic")) ==
                    request.graphicsTier &&
                requiredObject("resolution") && requiredObject("entry") &&
                requiredArray("supportedLocales") &&
                requiredArray("extensions") &&
                requiredObject("uiEntryPoints") &&
                canonicalProject.contains("storyIndex") &&
                canonicalProject["storyIndex"].is_string() &&
                canonicalProject.contains("gameCatalog") &&
                canonicalProject["gameCatalog"].is_string() &&
                canonicalProject.contains("defaultLocale") &&
                canonicalProject["defaultLocale"].is_string();
            if (projectValid) {
                const auto& resolution = canonicalProject["resolution"];
                const auto& entry = canonicalProject["entry"];
                projectValid = resolution.value("width", 0) == request.width &&
                    resolution.value("height", 0) == request.height &&
                    !entry.value("story", std::string{}).empty() &&
                    !entry.value("ui", std::string{}).empty();
            }
            if (!projectValid) {
                AddDiagnostic(
                    diagnostics, "PXPKG1240",
                    "project.pxproject is not the canonical 0.2 project contract or disagrees with the package request");
            } else {
                const std::string storyIndex =
                    canonicalProject["storyIndex"].get<std::string>();
                if (!IsSafeUri(storyIndex) ||
                    !storyIndex.ends_with(".pxindex") ||
                    !packagedInputs.contains(storyIndex)) {
                    AddDiagnostic(diagnostics, "PXPKG1241",
                                  "canonical project storyIndex is missing from package inputs");
                }
                const std::string defaultLocale =
                    canonicalProject["defaultLocale"].get<std::string>();
                bool defaultDeclared = false;
                std::set<std::string> localeIds;
                const auto localeCount = canonicalProject["supportedLocales"].size();
                std::size_t localeIndex = 0;
                for (const auto& locale : canonicalProject["supportedLocales"]) {
                    EmitProgress(request, eventSink, "validate-locales",
                                 localeIndex++, localeCount,
                                 locale.is_string()
                                     ? "Validating locale " + locale.get<std::string>()
                                     : "Validating locale descriptor");
                    if (!locale.is_string() ||
                        !IsLocaleId(locale.get<std::string>()) ||
                        !localeIds.insert(locale.get<std::string>()).second) {
                        AddDiagnostic(diagnostics, "PXPKG1242",
                                      "supportedLocales contains an invalid or duplicate locale");
                        continue;
                    }
                    const std::string id = locale.get<std::string>();
                    if (id == defaultLocale) defaultDeclared = true;
                    const std::string localePath =
                        "Content/Localization/" + id + ".json";
                    const auto localeText = readPackaged(localePath);
                    const Json localeDocument =
                        localeText ? Json::parse(*localeText, nullptr, false)
                                   : Json(Json::value_t::discarded);
                    if (!localeText || localeDocument.is_discarded() ||
                        !localeDocument.is_object() ||
                        localeDocument.value("format", std::string{}) !=
                            "PrismatiXLocale" ||
                        localeDocument.value("schemaRevision", 0) != 2 ||
                        localeDocument.value("locale", std::string{}) != id ||
                        !localeDocument.contains("strings") ||
                        !localeDocument["strings"].is_object()) {
                        AddDiagnostic(diagnostics, "PXPKG1243",
                                      "canonical locale is missing or has mismatched identity: " + id);
                    }
                    const std::string runtimePath =
                        "Runtime/Locales/" + id + "/main.pxir";
                    const std::string sourceMapPath =
                        "Runtime/Locales/" + id + "/main.pxmap";
                    const auto runtimeText = readPackaged(runtimePath);
                    const auto mapText = readPackaged(sourceMapPath);
                    const auto runtime = runtimeText
                                             ? ParseRuntimeIr(*runtimeText)
                                             : RuntimeIrParseResult{};
                    const auto map = mapText ? ParseSourceMap(*mapText)
                                             : SourceMapParseResult{};
                    if (!runtimeText || !mapText || !runtime.Valid() ||
                        !map.Valid() ||
                        runtime.document.documentId != map.document.documentId ||
                        runtime.document.operations.size() !=
                            map.document.mappings.size()) {
                        AddDiagnostic(
                            diagnostics, "PXPKG1260",
                            "locale RuntimeIR/source-map pair is missing or invalid: " +
                                id);
                    } else {
                        bool identitiesValid = true;
                        for (const auto& operation : runtime.document.operations) {
                            const auto* mapping =
                                map.document.Find(operation.operationId);
                            if (!mapping ||
                                mapping->sourceId != operation.sourceId ||
                                mapping->startLine != operation.sourceLine ||
                                !packagedInputs.contains(mapping->sourceUri)) {
                                identitiesValid = false;
                                break;
                            }
                        }
                        if (!identitiesValid) {
                            AddDiagnostic(
                                diagnostics, "PXPKG1261",
                                "locale RuntimeIR source identities are not fully packaged: " +
                                    id);
                        } else {
                            ValidateLocaleFontCoverage(
                                localeDocument, id, &runtime.document,
                                packagedInputs, diagnostics);
                            LocalizedProgram topology{id, runtime.document.documentId, {}};
                            topology.operations.reserve(runtime.document.operations.size());
                            for (const auto& operation : runtime.document.operations) {
                                topology.operations.push_back({operation.operationId,
                                                             operation.sourceId, operation.kind});
                            }
                            localizedPrograms.push_back(std::move(topology));
                            const auto primaryRuntime = packagedInputs.find(request.startScript);
                            const auto primaryMap = packagedInputs.find(request.sourceMap);
                            const auto localeRuntime = packagedInputs.find(runtimePath);
                            const auto localeMap = packagedInputs.find(sourceMapPath);
                            // Runtime IR and source maps are not deferred media:
                            // each has passed raw SHA-256 verification. Reuse only when
                            // the locale pair also covered every primary
                            // Runtime IR/source-map identity check, including sourceLine.
                            const auto sameValidatedInput = [](const ValidatedInput* left,
                                                               const ValidatedInput* right) {
                                return left->input.size == right->input.size &&
                                       Lower(left->input.fingerprint) == Lower(right->input.fingerprint);
                            };
                            if (!equivalentPrimaryLocale &&
                                primaryRuntime != packagedInputs.end() &&
                                primaryMap != packagedInputs.end() &&
                                localeRuntime != packagedInputs.end() &&
                                localeMap != packagedInputs.end() &&
                                sameValidatedInput(primaryRuntime->second, localeRuntime->second) &&
                                sameValidatedInput(primaryMap->second, localeMap->second)) {
                                equivalentPrimaryLocale = localizedPrograms.size() - 1;
                            }
                        }
                    }
                }
                if (localeCount != 0) {
                    EmitProgress(request, eventSink, "validate-locales",
                                 localeCount, localeCount,
                                 "Validated locale Runtime IR and source maps");
                }
                if (!defaultDeclared) {
                    AddDiagnostic(diagnostics, "PXPKG1244",
                                  "defaultLocale is not present in supportedLocales");
                }
                std::vector<std::string> projectExtensions;
                for (const auto& extension : canonicalProject["extensions"]) {
                    if (!extension.is_string() ||
                        !IsSafeUri(extension.get<std::string>()) ||
                        !extension.get<std::string>().ends_with(".pxextension") ||
                        !packagedInputs.contains(extension.get<std::string>())) {
                        AddDiagnostic(diagnostics, "PXPKG1245",
                                      "project extension is invalid or missing from package inputs");
                        continue;
                    }
                    const std::string manifestPath = extension.get<std::string>();
                    projectExtensions.push_back(manifestPath);
                    const auto manifestText = readPackaged(manifestPath);
                    const Json extensionManifest = manifestText
                        ? Json::parse(*manifestText, nullptr, false)
                        : Json(Json::value_t::discarded);
                    if (!manifestText || extensionManifest.is_discarded() ||
                        !extensionManifest.is_object() ||
                        extensionManifest.value("format", std::string{}) !=
                            "PrismatiXExtension" ||
                        extensionManifest.value("schemaRevision", 0) != 2 ||
                        extensionManifest.value("language", std::string{}) !=
                            "javascript" ||
                        !semver::Parse(extensionManifest.value(
                            "version", std::string{}))) {
                        AddDiagnostic(diagnostics, "PXPKG1247",
                                      "extension manifest is not a canonical 0.2 JavaScript extension: " + manifestPath);
                        continue;
                    }
                    const auto compatible = semver::Satisfies(
                        semver::Version{0, 2, 0},
                        extensionManifest.value("requiredEngineVersion",
                                                std::string{}));
                    if (!compatible || !*compatible) {
                        AddDiagnostic(diagnostics, "PXPKG1248",
                                      "extension requiredEngineVersion is invalid or incompatible: " + manifestPath);
                    }
                    const auto capabilities = extensionManifest.find("capabilities");
                    std::set<std::string> declaredCapabilities;
                    if (capabilities == extensionManifest.end() ||
                        !capabilities->is_array()) {
                        AddDiagnostic(diagnostics, "PXPKG1249",
                                      "extension capabilities must be an array: " + manifestPath);
                    } else {
                        for (const auto& capability : *capabilities) {
                            if (!capability.is_string() ||
                                !std::set<std::string>{"runtime", "animation", "ui", "audio",
                                                       "video", "persistence", "input", "render"}
                                     .contains(capability.get<std::string>()) ||
                                !declaredCapabilities.insert(
                                    capability.get<std::string>()).second) {
                                AddDiagnostic(diagnostics, "PXPKG1249",
                                              "extension capability is unsupported or duplicated: " + manifestPath);
                            }
                        }
                    }
                    const std::string entryPath =
                        extensionManifest.value("entry", std::string{});
                    const auto separator = manifestPath.find_last_of('/');
                    const std::string resolvedEntry =
                        separator == std::string::npos
                            ? entryPath
                            : manifestPath.substr(0, separator + 1) + entryPath;
                    if (!IsSafeUri(entryPath) || !entryPath.ends_with(".js") ||
                        !IsSafeUri(resolvedEntry) ||
                        !packagedInputs.contains(resolvedEntry)) {
                        AddDiagnostic(diagnostics, "PXPKG1250",
                                      "extension entry is unsafe or missing from package inputs: " + manifestPath);
                    }
                    const auto modules = extensionManifest.find("modules");
                    if (modules != extensionManifest.end()) {
                        if (!modules->is_array() || modules->size() > 1024) {
                            AddDiagnostic(diagnostics, "PXPKG1252",
                                          "extension modules must be a bounded array: " + manifestPath);
                        } else {
                            std::set<std::string> uniqueModules;
                            for (const auto& module : *modules) {
                                if (!module.is_string()) {
                                    AddDiagnostic(diagnostics, "PXPKG1252",
                                                  "extension module path must be a string: " + manifestPath);
                                    continue;
                                }
                                const std::string modulePath = module.get<std::string>();
                                const std::string resolvedModule =
                                    separator == std::string::npos
                                        ? modulePath
                                        : manifestPath.substr(0, separator + 1) + modulePath;
                                if (!IsSafeUri(modulePath) ||
                                    !modulePath.ends_with(".js") ||
                                    !IsSafeUri(resolvedModule) ||
                                    !uniqueModules.insert(resolvedModule).second ||
                                    !packagedInputs.contains(resolvedModule)) {
                                    AddDiagnostic(diagnostics, "PXPKG1252",
                                                  "extension module is unsafe, duplicated, or missing from package inputs: " +
                                                      manifestPath + " -> " + modulePath);
                                }
                            }
                        }
                    }
                }
                if (projectExtensions != request.extensions) {
                    AddDiagnostic(diagnostics, "PXPKG1251",
                                  "package request extensions do not exactly match project.pxproject");
                }
                for (auto ui = canonicalProject["uiEntryPoints"].begin();
                     ui != canonicalProject["uiEntryPoints"].end(); ++ui) {
                    if (!ui.value().is_string() ||
                        !IsSafeUri(ui.value().get<std::string>()) ||
                        !packagedInputs.contains(ui.value().get<std::string>())) {
                        AddDiagnostic(diagnostics, "PXPKG1246",
                                      "project UI entry point is invalid or missing from package inputs");
                    }
                }
                if (const auto components = canonicalProject.find("uiComponents");
                    components != canonicalProject.end()) {
                    if (!components->is_array() || components->size() > 100'000) {
                        AddDiagnostic(diagnostics, "PXPKG1257",
                                      "project uiComponents must be a bounded array");
                    } else {
                        std::set<std::string> componentIds;
                        for (const auto& descriptor : *components) {
                            if (!descriptor.is_object() ||
                                !descriptor.contains("id") ||
                                !descriptor["id"].is_string() ||
                                !descriptor.contains("source") ||
                                !descriptor["source"].is_string()) {
                                AddDiagnostic(diagnostics, "PXPKG1258",
                                              "UI component descriptor is malformed");
                                continue;
                            }
                            const std::string id = descriptor["id"].get<std::string>();
                            const std::string path = descriptor["source"].get<std::string>();
                            const auto source = packagedInputs.find(path);
                            if (!componentIds.insert(id).second ||
                                !IsSafeUri(path) || source == packagedInputs.end()) {
                                AddDiagnostic(diagnostics, "PXPKG1259",
                                              "UI component identity is duplicated or its source is missing");
                                continue;
                            }
                            const auto componentText = ReadVerifiedTextFile(
                                *source->second, diagnostics);
                            const auto component = componentText
                                ? ParseUiComponent(*componentText)
                                : UiComponentParseResult{};
                            if (!componentText) {
                                AddDiagnostic(diagnostics, "PXPKG1260",
                                              "UI component document could not be read: " + path,
                                              true);
                            } else if (!component.Valid()) {
                                for (const auto& issue : component.diagnostics)
                                    AddDiagnostic(diagnostics, issue.code,
                                                  issue.message, false, path);
                            } else if (component.document.content.id != id) {
                                AddDiagnostic(diagnostics, "PXPKG1260",
                                              "UI component document identity disagrees with its descriptor: " + path);
                            }
                        }
                    }
                }
            }
            EmitProgress(request, eventSink, "validate-characters", 0, 1,
                         "Validating character and expression resources");
            const auto characters = LoadCharacterResources(
                *projectText, readPackaged, packagedExists);
            EmitProgress(request, eventSink, "validate-characters", 1, 1,
                         "Validated character and expression resources");
            for (const auto& diagnostic : characters.diagnostics) {
                AddDiagnostic(
                    diagnostics, diagnostic.code,
                    diagnostic.message +
                        (diagnostic.path.empty()
                             ? std::string{}
                             : std::string(" [") + diagnostic.path + "]"));
            }
            if (characters.diagnostics.empty()) {
                const Json projectDocument = Json::parse(*projectText, nullptr, false);
                const std::string canonicalCatalog =
                    projectDocument.is_object()
                        ? projectDocument.value("gameCatalog", std::string{})
                        : std::string{};
                if (!canonicalCatalog.empty()) {
                    const auto catalog = packagedInputs.find(canonicalCatalog);
                    if (!IsSafeUri(canonicalCatalog) ||
                        !canonicalCatalog.ends_with(".pxgame") ||
                        catalog == packagedInputs.end()) {
                        AddDiagnostic(diagnostics, "PXPKG1230",
                                      "canonical project gameCatalog must identify a packaged .pxgame document");
                    } else {
                        const auto catalogText = ReadVerifiedTextFile(
                            *catalog->second, diagnostics);
                        if (!catalogText) {
                            AddDiagnostic(diagnostics, "PXPKG1231",
                                          "canonical gameCatalog could not be read", true);
                        } else {
                            auto runtimeCatalog = LoadCanonicalGameCatalogResources(
                                *catalogText, canonicalCatalog);
                            if (runtimeCatalog.Valid() &&
                                !runtimeCatalog.document.gallery.empty()) {
                                runtimeCatalog = ResolveGameCatalogGalleryResources(
                                    std::move(runtimeCatalog.document), *projectText,
                                    packagedExists, canonicalCatalog);
                            }
                            for (const auto& diagnostic : runtimeCatalog.diagnostics)
                                AddDiagnostic(diagnostics, diagnostic.code,
                                              diagnostic.message);
                        }
                    }
                } else {
                    AddDiagnostic(
                        diagnostics, "PXPKG1228",
                        "0.2 project must declare a canonical gameCatalog .pxgame document");
                }
            }
        }
    }
    std::optional<RuntimeIrDocument> packagedRuntimeIr;
    std::optional<SourceMapDocument> packagedSourceMap;
    if (!uris.contains(request.startScript)) {
        AddDiagnostic(diagnostics, "PXPKG1217", "startScript must be present in inputs");
    } else if (!equivalentPrimaryLocale) {
        const auto script = std::find_if(result.inputs.begin(), result.inputs.end(), [&](const auto& input) { return input.input.uri == request.startScript; });
        if (script != result.inputs.end()) {
            const auto text = ReadVerifiedTextFile(*script, diagnostics);
            const auto parsed = text ? ParseRuntimeIr(*text)
                                     : RuntimeIrParseResult{};
            if (!text || !parsed.Valid()) {
                AddDiagnostic(diagnostics, "PXPKG1223", "startScript is not valid compiled Runtime IR");
            } else {
                packagedRuntimeIr = parsed.document;
            }
        }
    }
    if (!uris.contains(request.sourceMap)) {
        AddDiagnostic(diagnostics, "PXPKG1238",
                      "sourceMap must be present in inputs");
    } else if (!equivalentPrimaryLocale) {
        const auto sourceMap = std::find_if(
            result.inputs.begin(), result.inputs.end(), [&](const auto& input) {
                return input.input.uri == request.sourceMap;
            });
        const auto text = sourceMap == result.inputs.end()
                               ? std::optional<std::string>{}
                               : ReadVerifiedTextFile(*sourceMap, diagnostics);
        const auto parsed = text ? ParseSourceMap(*text)
                                 : SourceMapParseResult{};
        if (!text || !parsed.Valid()) {
            AddDiagnostic(diagnostics, "PXPKG1239",
                          "sourceMap is not a valid canonical source map");
            for (const auto& issue : parsed.diagnostics) {
                AddDiagnostic(diagnostics, issue.code, issue.message);
            }
        } else {
            packagedSourceMap = parsed.document;
        }
    }
    if (packagedRuntimeIr && packagedSourceMap) {
        if (packagedRuntimeIr->documentId != packagedSourceMap->documentId) {
            AddDiagnostic(diagnostics, "PXPKG1252",
                          "sourceMap documentId does not match startScript Runtime IR");
        }
        if (packagedRuntimeIr->operations.size() !=
            packagedSourceMap->mappings.size()) {
            AddDiagnostic(diagnostics, "PXPKG1253",
                          "sourceMap must contain exactly one mapping per Runtime IR operation");
        }
        for (const auto& operation : packagedRuntimeIr->operations) {
            const auto* mapping = packagedSourceMap->Find(operation.operationId);
            if (!mapping) {
                AddDiagnostic(diagnostics, "PXPKG1254",
                              "sourceMap is missing operationId: " + operation.operationId);
                continue;
            }
            if (mapping->sourceId != operation.sourceId ||
                mapping->startLine != operation.sourceLine) {
                AddDiagnostic(diagnostics, "PXPKG1255",
                              "sourceMap identity or source line disagrees with Runtime IR: " +
                                  operation.operationId);
            }
            if (!uris.contains(mapping->sourceUri)) {
                AddDiagnostic(diagnostics, "PXPKG1256",
                              "sourceMap sourceUri is not packaged: " +
                                  mapping->sourceUri);
            }
        }
    }
    const auto validateLocalizedTopology =
        [&](const std::string_view documentId, const auto& canonicalOperations) {
        for (const auto& localized : localizedPrograms) {
            if (localized.documentId != documentId ||
                localized.operations.size() != canonicalOperations.size()) {
                AddDiagnostic(diagnostics, "PXPKG1262",
                              "locale Story topology differs from package entry: " +
                                  localized.locale);
                continue;
            }
            for (std::size_t index = 0;
                 index < canonicalOperations.size(); ++index) {
                const auto& canonical = canonicalOperations[index];
                const auto& translated = localized.operations[index];
                if (canonical.operationId != translated.operationId ||
                    canonical.sourceId != translated.sourceId ||
                    canonical.kind != translated.kind) {
                    AddDiagnostic(
                        diagnostics, "PXPKG1263",
                        "locale Story stable identity differs at operation " +
                            std::to_string(index) + ": " + localized.locale);
                    break;
                }
            }
        }
    };
    if (equivalentPrimaryLocale) {
        const auto& canonical = localizedPrograms[*equivalentPrimaryLocale];
        validateLocalizedTopology(canonical.documentId, canonical.operations);
        EmitProgress(request, eventSink, "validate-primary-reuse", 1, 1,
                     "Reused verified locale Runtime IR and source map for package entry");
    } else if (packagedRuntimeIr && packagedSourceMap) {
        validateLocalizedTopology(packagedRuntimeIr->documentId,
                                  packagedRuntimeIr->operations);
    }
    std::set<std::string> routeIds;
    for (const auto& route : request.routes) {
        if (route.id.empty() || route.id.size() > 128 || !std::all_of(route.id.begin(), route.id.end(), [](const unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.'; })) {
            AddDiagnostic(diagnostics, "PXPKG1218", "route id is invalid");
        }
        else if (!routeIds.insert(route.id).second) {
            AddDiagnostic(diagnostics, "PXPKG1219", "route id is duplicated: " + route.id);
        }
        if (!IsSafeUri(route.scene) ||
            (!route.scene.ends_with(".pxscene") &&
             !route.scene.ends_with(".pxui"))) {
            AddDiagnostic(diagnostics, "PXPKG1220", "route scene must be a project-relative .pxscene or .pxui URI: " + route.scene);
            continue;
        }
        if (!uris.contains(route.scene)) {
            AddDiagnostic(diagnostics, "PXPKG1221", "route scene must be present in inputs: " + route.scene);
        }
        else {
            const auto scene = std::find_if(result.inputs.begin(), result.inputs.end(), [&](const auto& input) { return input.input.uri == route.scene; });
            if (scene != result.inputs.end()) {
                const auto text = ReadVerifiedTextFile(*scene, diagnostics);
                if (!text) {
                    AddDiagnostic(diagnostics, "PXPKG1224", "route scene is neither a typed UIScene nor UI document: " + route.scene);
                } else if (route.scene.ends_with(".pxui")) {
                    const auto parsed = ParseUi(*text);
                    if (!parsed.Valid()) {
                        for (const auto& issue : parsed.diagnostics)
                            AddDiagnostic(diagnostics, issue.code,
                                          issue.message, false, route.scene);
                    }
                } else if (!PlayerUiSceneId(*text)) {
                    AddDiagnostic(diagnostics, "PXPKG1224",
                                  "route scene is not a typed UIScene: " +
                                      route.scene);
                }
            }
        }
    }
    if (!request.startRoute.empty() && !routeIds.contains(request.startRoute)) {
        AddDiagnostic(diagnostics, "PXPKG1222", "startRoute must identify one of the packaged routes");
    }
    EmitProgress(request, eventSink, "validate-structure", 1, 1,
                 "Validated project, Story, UI, and resource contracts");
    return result;
}

std::filesystem::path UniqueSibling(const std::filesystem::path& output, const std::string_view label, const std::string_view requestId) {
    const auto digest = Sha256Text(std::string(requestId) + "|" + px::PathToUtf8(output) + "|" + std::string(label));
    const std::string suffix = digest ? Hex(*digest).substr(0, 16) : std::string("0000000000000000");
    return output.parent_path() / Utf8Path(px::PathToUtf8(output.filename()) + "." + std::string(label) + "-" + suffix);
}

}  // namespace

std::string detail::SerializePackageManifest(
    const detail::PackageManifest& manifest) {
    Json routes = Json::array();
    for (const auto& route : manifest.routes) {
        routes.push_back({{"id", route.id}, {"sceneId", route.sceneId},
                          {"scene", route.scene}});
    }
    Json archives = Json::array();
    for (const auto& archive : manifest.archives) {
        archives.push_back({{"file", archive.file}, {"group", archive.group},
                            {"optional", archive.optional}});
    }
    Json migrations = Json::array();
    for (const auto& migration : manifest.saveMigrations) {
        migrations.push_back(
            {{"id", migration.id},
             {"from", {{"contentVersion", migration.fromContentVersion},
                       {"saveVersion", migration.fromSaveVersion}}},
             {"to", {{"contentVersion", migration.toContentVersion},
                     {"saveVersion", migration.toSaveVersion}}},
             {"asset", migration.asset}});
    }
    const Json extensions = manifest.extensions;
    Json customEffects = Json::array();
    for (const auto& effect : manifest.customEffects) {
        Json uniforms = Json::array();
        for (const auto& uniform : effect.uniforms) {
            Json defaultValue = uniform.defaultValue[0];
            if (uniform.type == "vec2")
                defaultValue = Json::array(
                    {uniform.defaultValue[0], uniform.defaultValue[1]});
            else if (uniform.type == "color")
                defaultValue = Json::array(
                    {uniform.defaultValue[0], uniform.defaultValue[1],
                     uniform.defaultValue[2], uniform.defaultValue[3]});
            uniforms.push_back({{"name", uniform.name},
                                {"type", uniform.type},
                                {"slot", uniform.slot},
                                {"default", std::move(defaultValue)},
                                {"minimum", uniform.minimum},
                                {"maximum", uniform.maximum}});
        }
        Json artifacts = Json::array();
        for (const auto& artifact : effect.artifacts)
            artifacts.push_back({{"format", artifact.format},
                                 {"asset", artifact.asset},
                                 {"fingerprint", artifact.fingerprint}});
        customEffects.push_back(
            {{"id", effect.id},
             {"schemaRevision", effect.schemaRevision},
             {"targetLayer", effect.targetLayer},
             {"uniforms", std::move(uniforms)},
             {"artifacts", std::move(artifacts)},
             {"reflection", {{"samplers", effect.samplerCount},
                              {"uniformBuffers", effect.uniformBufferCount},
                              {"storageTextures", 0},
                              {"storageBuffers", 0}}}});
    }
    Json root{
        {"format", "PrismatiXPackageManifest"},
        {"schemaRevision", 2},
        {"engineVersion", manifest.engineVersion},
        {"gameId", manifest.gameId},
        {"title", manifest.title},
        {"resolution", {{"width", manifest.width}, {"height", manifest.height}}},
        {"entry", {{"runtimeIr", manifest.startRuntimeIr},
                   {"sourceMap", manifest.sourceMap},
                   {"route", manifest.startRoute}}},
        {"routes", std::move(routes)},
        {"archives", std::move(archives)},
        {"saveMigrations", std::move(migrations)},
        {"extensions", extensions},
        {"customEffects", std::move(customEffects)},
        {"contentVersion", manifest.contentVersion},
        {"saveVersion", manifest.saveVersion},
        {"packageFingerprint", manifest.packageFingerprint},
        {"persistence",
         {{"dataId", manifest.persistenceDataId},
          {"keyId", manifest.persistenceKeyId},
          {"legacyReaders", [&manifest] {
               Json readers = Json::array();
               for (const auto& reader :
                    manifest.legacyPersistenceReaders) {
                   readers.push_back({{"keyId", reader.keyId},
                                      {"secret", reader.secret}});
               }
               return readers;
           }()},
          {"compatibleSaveFingerprints",
           manifest.compatibleSaveFingerprints}}},
        {"encryption", {{"enabled", manifest.encrypted},
                        {"archiveKey", manifest.archiveKey}}},
        {"graphicsTier", manifest.graphicsTier}};
    if (!manifest.publisherPublicKey.empty())
        root["publisherPublicKey"] = manifest.publisherPublicKey;
    return root.dump(2) + "\n";
}

PackageRequestParseResult ParsePackageRequest(const std::string_view text) {
    PackageRequestParseResult result;
    if (text.size() > 16 * 1024 * 1024) {
        AddDiagnostic(result.diagnostics, "PXPKG1099", "Package request exceeds the 16 MiB contract limit");
        return result;
    }
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        AddDiagnostic(result.diagnostics, "PXPKG1100", "Package request must be a JSON object");
        return result;
    }
    const auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != "PrismatiXPackageRequest") {
        AddDiagnostic(result.diagnostics, "PXPKG1102", "format must be PrismatiXPackageRequest");
    }
    const auto revision = root.find("schemaRevision");
    const auto revisionValue = revision == root.end() ? std::optional<int>{} : JsonInt(*revision);
    if (!revisionValue || *revisionValue != 2) {
        AddDiagnostic(result.diagnostics, "PXPKG1103", "schemaRevision must be 2");
    }
    RequiredString(root, "requestId", result.request.requestId, result.diagnostics);
    RequiredString(root, "gameId", result.request.gameId, result.diagnostics);
    result.request.projectRoot = JsonPath(root, "projectRoot");
    result.request.outputDir = JsonPath(root, "outputDir");
    const auto cacheSource = root.find("cacheSourceDir");
    if (cacheSource != root.end()) {
        if (!cacheSource->is_string() || cacheSource->empty()) {
            AddDiagnostic(result.diagnostics, "PXPKG1240",
                          "cacheSourceDir must be a non-empty path string when provided");
        } else {
            result.request.cacheSourceDir = JsonPath(root, "cacheSourceDir");
        }
    }
    const auto retireCacheSource =
        root.find("retireCacheSourceAfterPromotion");
    if (retireCacheSource != root.end()) {
        if (!retireCacheSource->is_boolean())
            AddDiagnostic(result.diagnostics, "PXPKG1242",
                          "retireCacheSourceAfterPromotion must be a boolean");
        else
            result.request.retireCacheSourceAfterPromotion =
                retireCacheSource->get<bool>();
    }
    result.request.playerExecutable = JsonPath(root, "playerExecutable");
    result.request.cancelFile = JsonPath(root, "cancelFile");
    RequiredString(root, "title", result.request.title, result.diagnostics);
    RequiredString(root, "startScript", result.request.startScript, result.diagnostics);
    RequiredString(root, "sourceMap", result.request.sourceMap, result.diagnostics);
    RequiredString(root, "contentVersion", result.request.contentVersion, result.diagnostics);
    RequiredString(root, "graphicsTier", result.request.graphicsTier,
                   result.diagnostics);
    if (const auto publisherKey = root.find("publisherPublicKey");
        publisherKey != root.end()) {
        if (!publisherKey->is_string())
            AddDiagnostic(result.diagnostics, "PXPKG1243",
                          "publisherPublicKey must be a string");
        else
            result.request.publisherPublicKey = publisherKey->get<std::string>();
    }
    if (result.request.graphicsTier != "basic" &&
        result.request.graphicsTier != "gpu-effects")
        AddDiagnostic(result.diagnostics, "PXPKG1120",
                      "graphicsTier must be basic or gpu-effects");
    const auto saveVersion = root.find("saveVersion");
    const auto saveVersionValue = saveVersion == root.end()
                                      ? std::optional<int>{}
                                      : JsonInt(*saveVersion);
    if (!saveVersionValue || *saveVersionValue <= 0) {
        AddDiagnostic(result.diagnostics, "PXPKG1114",
                      "saveVersion must be a positive integer");
    } else {
        result.request.saveVersion = static_cast<std::uint32_t>(*saveVersionValue);
    }

    const auto startRoute = root.find("startRoute");
    if (startRoute == root.end() || !startRoute->is_string()) {
        AddDiagnostic(result.diagnostics, "PXPKG1104", "startRoute must be a string");
    }
    else {
        result.request.startRoute = startRoute->get<std::string>();
    }
    for (const char* key : { "projectRoot", "outputDir", "playerExecutable", "cancelFile" }) {
        const auto value = root.find(key);
        if (value == root.end() || !value->is_string() || value->empty()) {
            AddDiagnostic(result.diagnostics, "PXPKG1105", std::string(key) + " must be a non-empty path string");
        }
    }
    const auto width = root.find("width");
    const auto height = root.find("height");
    const auto widthValue = width == root.end() ? std::optional<int>{} : JsonInt(*width);
    const auto heightValue = height == root.end() ? std::optional<int>{} : JsonInt(*height);
    if (!widthValue || !heightValue) {
        AddDiagnostic(result.diagnostics, "PXPKG1106", "width and height must be integers");
    }
    else {
        result.request.width = *widthValue;
        result.request.height = *heightValue;
    }
    const auto encryption = root.find("encryption");
    if (encryption == root.end() || !encryption->is_boolean()) {
        AddDiagnostic(result.diagnostics, "PXPKG1107", "encryption must be a boolean");
    }
    else {
        result.request.encryption = encryption->get<bool>();
    }
    const auto compression = root.find("compression");
    if (compression == root.end() || !compression->is_string()) {
        AddDiagnostic(result.diagnostics, "PXPKG1108", "compression must be none, fast, balanced, or maximum");
    }
    else {
        const std::string value = compression->get<std::string>();
        if (value == "none")
            result.request.compression = PackageCompression::None;
        else if (value == "fast") {
            result.request.compression = PackageCompression::Fast;
        }
        else if (value == "balanced") {
            result.request.compression = PackageCompression::Balanced;
        }
        else if (value == "maximum") {
            result.request.compression = PackageCompression::Maximum;
        }
        else {
            AddDiagnostic(result.diagnostics, "PXPKG1108", "compression must be none, fast, balanced, or maximum");
        }
    }

    const auto archiveShards = root.find("archiveShards");
    if (archiveShards != root.end()) {
        const auto count = JsonInt(*archiveShards);
        if (!count || *count < 1 || *count > 64) {
            AddDiagnostic(result.diagnostics, "PXPKG1241",
                          "archiveShards must be an integer from 1 to 64");
        } else {
            result.request.archiveShards = static_cast<std::uint32_t>(*count);
        }
    }

    const auto inputs = root.find("inputs");
    if (inputs == root.end() || !inputs->is_array()) {
        AddDiagnostic(result.diagnostics, "PXPKG1109", "inputs must be an array");
    }
    else if (inputs->size() > kMaxInputs) {
        AddDiagnostic(result.diagnostics, "PXPKG1113", "inputs exceeds the package contract limit");
    }
    else {
        result.request.inputs.reserve(inputs->size());
        for (const auto& item : *inputs) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string() || !item.contains("fingerprint") || !item["fingerprint"].is_string() || !item.contains("size") || !item["size"].is_number_unsigned()) {
                AddDiagnostic(result.diagnostics, "PXPKG1110", "each input requires uri, fingerprint, and unsigned size");
                continue;
            }
            result.request.inputs.push_back({ item["uri"].get<std::string>(), item["fingerprint"].get<std::string>(), item["size"].get<std::uint64_t>() });
        }
    }

    const auto routes = root.find("routes");
    if (routes != root.end()) {
        if (!routes->is_array()) {
            AddDiagnostic(result.diagnostics, "PXPKG1111", "routes must be an array when present");
        }
        else {
            for (const auto& item : *routes) {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_string() || !item.contains("scene") || !item["scene"].is_string()) {
                    AddDiagnostic(result.diagnostics, "PXPKG1112", "each route requires string id and scene");
                    continue;
                }
                result.request.routes.push_back({ item["id"].get<std::string>(), item["scene"].get<std::string>() });
            }
        }
    }
    const auto extensions = root.find("extensions");
    if (extensions == root.end() || !extensions->is_array() ||
        extensions->size() > 1024) {
        AddDiagnostic(result.diagnostics, "PXPKG1118",
                      "extensions must be a bounded array");
    } else {
        std::set<std::string> paths;
        for (const auto& value : *extensions) {
            if (!value.is_string() ||
                !IsSafeUri(value.get<std::string>()) ||
                !value.get<std::string>().ends_with(".pxextension") ||
                !paths.insert(value.get<std::string>()).second) {
                AddDiagnostic(result.diagnostics, "PXPKG1119",
                              "extension path is unsafe or duplicated");
                continue;
            }
            result.request.extensions.push_back(value.get<std::string>());
        }
    }
    const auto migrations = root.find("saveMigrations");
    if (migrations != root.end()) {
        if (!migrations->is_array() || migrations->size() > 64) {
            AddDiagnostic(result.diagnostics, "PXPKG1115",
                          "saveMigrations must be an array with at most 64 entries");
        } else {
            for (const auto& item : *migrations) {
                if (!item.is_object() || !item.contains("from") ||
                    !item["from"].is_object() || !item.contains("to") ||
                    !item["to"].is_object()) {
                    AddDiagnostic(result.diagnostics, "PXPKG1116",
                                  "each save migration requires from and to objects");
                    continue;
                }
                PackageSaveMigration migration;
                migration.id = item.value("id", std::string{});
                migration.asset = item.value("asset", std::string{});
                migration.fromContentVersion =
                    item["from"].value("contentVersion", std::string{});
                migration.fromSaveVersion =
                    item["from"].value("saveVersion", std::uint32_t{0});
                migration.toContentVersion =
                    item["to"].value("contentVersion", std::string{});
                migration.toSaveVersion =
                    item["to"].value("saveVersion", std::uint32_t{0});
                result.request.saveMigrations.push_back(std::move(migration));
            }
        }
    }
    return result;
}

std::string SerializePackageEvent(const PackageEvent& event) {
    Json object{ { "protocolVersion", 1 }, { "requestId", event.requestId } };
    switch (event.kind) {
        case PackageEventKind::Progress:
            object["event"] = "progress";
            object["phase"] = event.phase;
            object["current"] = event.current;
            object["total"] = event.total;
            object["message"] = event.message;
            break;
        case PackageEventKind::Completed:
            object["event"] = "completed";
            object["outputDir"] = px::PathToUtf8(event.outputDir);
            object["playerExecutable"] = px::PathToUtf8(event.playerExecutable);
            object["manifestPath"] = px::PathToUtf8(event.manifestPath);
            object["inputCount"] = event.inputCount;
            break;
        case PackageEventKind::Validated:
            object["event"] = "validated";
            object["inputCount"] = event.inputCount;
            object["deferredMediaCount"] = event.deferredMediaCount;
            break;
        case PackageEventKind::Cancelled:
            object["event"] = "cancelled";
            object["message"] = event.message;
            break;
        case PackageEventKind::Failed:
            object["event"] = "failed";
            object["code"] = event.code;
            object["message"] = event.message;
            object["retryable"] = event.retryable;
            {
                const auto diagnosticJson = [](const PackageDiagnostic& diagnostic) {
                    Json span = nullptr;
                    if (diagnostic.span) {
                        span = {{"path", diagnostic.span->path},
                                {"start", {{"line", diagnostic.span->start.line},
                                           {"column", diagnostic.span->start.column},
                                           {"offset", diagnostic.span->start.offset}}},
                                {"end", {{"line", diagnostic.span->end.line},
                                         {"column", diagnostic.span->end.column},
                                         {"offset", diagnostic.span->end.offset}}}};
                    }
                    return Json{{"severity", diagnostic.severity},
                                {"code", diagnostic.code},
                                {"message", diagnostic.message},
                                {"documentId", diagnostic.documentId},
                                {"sourceId", diagnostic.sourceId},
                                {"span", std::move(span)},
                                {"hint", diagnostic.hint},
                                {"cause", diagnostic.cause},
                                {"retryable", diagnostic.retryable}};
                };
                std::vector<PackageDiagnostic> diagnostics = event.diagnostics;
                if (diagnostics.empty())
                    diagnostics.push_back(
                        {event.code, event.message, event.retryable});
                object["diagnostic"] = diagnosticJson(diagnostics.front());
                object["diagnostics"] = Json::array();
                for (const auto& diagnostic : diagnostics)
                    object["diagnostics"].push_back(diagnosticJson(diagnostic));
                const auto& primary = diagnostics.front();
                object["severity"] = primary.severity;
                object["documentId"] = primary.documentId;
                object["sourceId"] = primary.sourceId;
                object["span"] = object["diagnostic"]["span"];
                object["hint"] = primary.hint;
                object["cause"] = primary.cause;
            }
            break;
    }
    return object.dump();
}

std::string ComputePackageFingerprint(const std::filesystem::path& path) {
    if (!CryptoReady()) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};

    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) return {};
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && psa_hash_update(&operation, buffer.data(), static_cast<std::size_t>(count)) != PSA_SUCCESS) {
            psa_hash_abort(&operation);
            return {};
        }
    }
    if (!input.eof()) {
        psa_hash_abort(&operation);
        return {};
    }
    Key digest{};
    std::size_t written = 0;
    if (psa_hash_finish(&operation, digest.data(), digest.size(), &written) != PSA_SUCCESS || written != digest.size()) {
        psa_hash_abort(&operation);
        return {};
    }
    return Hex(digest);
}

PackageRunResult ValidatePackageRequest(const PackageRequest& request,
                                        const PackageEventSink& eventSink,
                                        const PackageValidationDepth depth) {
    EmitProgress(request, eventSink, "validate", 0, request.inputs.size(),
                 "Validating package request");
    auto validated = Validate(request, eventSink, depth);
    if (validated.cancelled)
        return Cancel(request, eventSink,
                      "Package validation was cancelled while checking inputs");
    if (!validated.diagnostics.empty())
        return Fail(request, eventSink, std::move(validated.diagnostics));
    if (Cancelled(request))
        return Cancel(request, eventSink,
                      "Package validation was cancelled after checking inputs");
    PackageEvent completed;
    completed.kind = PackageEventKind::Validated;
    completed.requestId = request.requestId;
    completed.inputCount = validated.inputs.size();
    completed.deferredMediaCount = validated.deferredMediaCount;
    Emit(eventSink, std::move(completed));
    PackageRunResult result;
    result.exitCode = PackageExitCode::Completed;
    return result;
}

// Keep the package identity and resource inventory independent of archive
// writing. A content-only update can use this same plan without first writing
// a second complete distribution.
struct PackageContentPlan {
    std::vector<ArchiveEntry> entries;
    std::vector<detail::PackageCustomEffect> customEffects;
    std::vector<detail::PackagePersistenceReader> legacyPersistenceReaders;
    std::vector<std::string> compatibleSaveFingerprints;
    std::string packageFingerprint;
    std::string encryptionKey;
};

bool PreparePackageContent(const PackageRequest& request,
                           const PackageEventSink& eventSink,
                           std::vector<ValidatedInput> inputs,
                           PreviousPersistence previousPersistence,
                           PackageContentPlan& plan,
                           PackageRunResult& failure) {
    const auto reject = [&](std::string code, std::string message,
                            const bool retryable = false) {
        failure = Fail(request, eventSink, std::move(code),
                       std::move(message), retryable);
        return false;
    };
    std::sort(inputs.begin(), inputs.end(), [](const auto& left, const auto& right) {
        return left.input.uri < right.input.uri;
    });
    auto& entries = plan.entries;
    entries.reserve(inputs.size());
    std::string identityMaterial =
        "PrismatiXPackageManifest|2|0.2.0|" + request.gameId + "|" +
        request.contentVersion + "|" + std::to_string(request.saveVersion) + "|" +
        request.startScript + "|" + request.sourceMap + "|" + request.startRoute + "|" +
        request.graphicsTier + "|" +
        std::to_string(request.width) + "x" + std::to_string(request.height);
    auto identityRoutes = request.routes;
    std::sort(identityRoutes.begin(), identityRoutes.end(), [](const auto& left,
                                                               const auto& right) {
        return left.id < right.id;
    });
    for (const auto& route : identityRoutes)
        identityMaterial += "|route|" + route.id + "|" + route.scene;
    for (const auto& extension : request.extensions)
        identityMaterial += "|extension|" + extension;
    if (!request.publisherPublicKey.empty())
        identityMaterial += "|publisher-key|" + request.publisherPublicKey;
    auto identityMigrations = request.saveMigrations;
    std::sort(identityMigrations.begin(), identityMigrations.end(),
              [](const auto& left, const auto& right) {
                  return left.id < right.id;
              });
    for (const auto& migration : identityMigrations) {
        identityMaterial +=
            "|save-migration|" + migration.id + "|" +
            migration.fromContentVersion + "|" +
            std::to_string(migration.fromSaveVersion) + "|" +
            migration.toContentVersion + "|" +
            std::to_string(migration.toSaveVersion) + "|" +
            migration.asset;
    }
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (Cancelled(request)) {
            failure = Cancel(request, eventSink,
                             "Packaging was cancelled while reading inputs");
            return false;
        }
        const auto& input = inputs[index];
        EmitProgress(request, eventSink, "archive", index, inputs.size(),
                     "Preparing " + input.input.uri);
        // Publishing defers source hashing until a verified read, archive
        // writing, or whole-shard reuse. JSON bytes also influence package
        // identity, so this read must match the declared fingerprint.
        std::string contentFingerprint = Lower(input.input.fingerprint);
        if (input.input.size <= 16ull * 1024ull * 1024ull &&
            CouldBeJson(input.source)) {
            const auto bytes = ReadFile(input.source, input.input.size);
            if (!bytes)
                return reject("PXPKG1305",
                              "An input changed while packaging: " + input.input.uri,
                              true);
            const auto sourceDigest = Sha256Bytes(bytes->data(), bytes->size());
            if (!sourceDigest ||
                Hex(*sourceDigest) != Lower(input.input.fingerprint))
                return reject("PXPKG1305",
                              "An input changed while packaging: " + input.input.uri,
                              true);
            const Json normalized = Json::parse(
                std::string(bytes->begin(), bytes->end()), nullptr, false);
            if (!normalized.is_discarded()) {
                const std::string canonical = normalized.dump();
                if (const auto digest = Sha256Text(canonical))
                    contentFingerprint = Hex(*digest);
            }
        }
        entries.push_back(ArchiveEntry{.uri = input.input.uri,
                                       .source = input.source,
                                       .size = input.input.size,
                                       .fingerprint =
                                           Lower(input.input.fingerprint)});
        identityMaterial += "|" + input.input.uri + "|" + contentFingerprint;
    }
    auto customEffects = CompileCustomEffects(entries);
    if (!customEffects.diagnostics.empty()) {
        const auto& first = customEffects.diagnostics.front();
        return reject(first.code, first.message, first.retryable);
    }
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return customEffects.sourceAssets.contains(entry.uri);
        }),
        entries.end());
    for (auto& artifact : customEffects.artifacts) {
        if (FindArchiveEntry(entries, artifact.uri))
            return reject("PXPKG1511",
                          "Generated shader artifact collides with a project input: " +
                              artifact.uri);
        const auto digest = Sha256Bytes(artifact.data.data(), artifact.data.size());
        if (!digest) return reject("PXPKG1306", "SHA-256 is unavailable");
        artifact.fingerprint = Hex(*digest);
        identityMaterial += "|shader-artifact|" + artifact.uri + "|" + artifact.fingerprint;
        entries.push_back(std::move(artifact));
    }
    std::ranges::sort(entries, {}, &ArchiveEntry::uri);
    const auto packageDigest = Sha256Text(identityMaterial);
    if (!packageDigest) return reject("PXPKG1306", "SHA-256 is unavailable");
    plan.packageFingerprint = Hex(*packageDigest);
    auto& compatibleSaveFingerprints = plan.compatibleSaveFingerprints;
    compatibleSaveFingerprints =
        std::move(previousPersistence.compatibleSaveFingerprints);
    if (previousPersistence.contentVersion != request.contentVersion ||
        previousPersistence.saveVersion != request.saveVersion) {
        compatibleSaveFingerprints.clear();
    } else if (!previousPersistence.packageFingerprint.empty() &&
               previousPersistence.packageFingerprint != plan.packageFingerprint) {
        compatibleSaveFingerprints.push_back(
            previousPersistence.packageFingerprint);
    }
    std::erase(compatibleSaveFingerprints, plan.packageFingerprint);
    std::ranges::sort(compatibleSaveFingerprints);
    compatibleSaveFingerprints.erase(
        std::unique(compatibleSaveFingerprints.begin(),
                    compatibleSaveFingerprints.end()),
        compatibleSaveFingerprints.end());
    if (compatibleSaveFingerprints.size() > 64)
        return reject("PXPKG1428",
                      "The same-version save fingerprint history exceeds 64 releases");
    plan.legacyPersistenceReaders = std::move(previousPersistence.readers);
    plan.customEffects = std::move(customEffects.manifests);
    plan.encryptionKey = "pxpkg-" + Hex(DeriveKey(
        "PrismatiXPackage|" + identityMaterial));
    return true;
}

bool WriteContentIndex(std::ostream& output, const PackageRequest& request,
                       const std::string_view packageFingerprint,
                       const std::vector<ArchiveEntry>& entries) {
    output << "{\"format\":\"PrismatiXContentIndex\",\"schemaRevision\":1,"
           << "\"gameId\":" << Json(request.gameId).dump()
           << ",\"packageFingerprint\":" << Json(packageFingerprint).dump()
           << ",\"entries\":[";
    bool firstEntry = true;
    for (const auto& entry : entries) {
        if (!firstEntry) output << ',';
        firstEntry = false;
        output << Json{
            {"uri", entry.uri},
            {"fingerprint", entry.fingerprint},
            {"size", entry.size},
            {"archive", ShardArchiveName(
                ArchiveShardForUri(entry.uri, request.archiveShards),
                request.archiveShards)}}.dump();
        if (!output || output.tellp() >
                static_cast<std::streamoff>(128ull * 1024ull * 1024ull))
            return false;
    }
    output << "]}";
    output.flush();
    return output && output.tellp() <=
        static_cast<std::streamoff>(128ull * 1024ull * 1024ull);
}

PackagePlanResult PlanPackageRequest(const PackageRequest& request,
                                     const PackageEventSink& eventSink,
                                     const PackageValidationDepth depth,
                                     const std::string_view previousManifestJson) {
    PackagePlanResult result;
    EmitProgress(request, eventSink, "validate", 0, request.inputs.size(),
                 "Validating package request for a content plan");
    auto validated = Validate(request, eventSink, depth,
                              depth == PackageValidationDepth::Structural);
    if (validated.cancelled) {
        result.diagnostics.push_back({"PXPKG1300", "Package planning was cancelled"});
        return result;
    }
    if (!validated.diagnostics.empty()) {
        result.diagnostics = std::move(validated.diagnostics);
        return result;
    }
    const auto persistenceSource = request.cacheSourceDir.empty()
        ? request.outputDir : request.cacheSourceDir;
    auto previousPersistence = previousManifestJson.empty()
        ? DiscoverPreviousPersistence(persistenceSource, request.gameId)
        : ParsePreviousPersistence(previousManifestJson, request.gameId);
    if (!previousManifestJson.empty() &&
        previousPersistence.packageFingerprint.empty() &&
        previousPersistence.error.empty())
        previousPersistence.error = "Previous package belongs to a different game";
    if (!previousPersistence.error.empty()) {
        result.diagnostics.push_back(
            {"PXPKG1317", std::move(previousPersistence.error), true});
        return result;
    }
    result.previousManifestSha256 = previousPersistence.manifestSha256;
    PackageContentPlan plan;
    PackageRunResult failure;
    if (!PreparePackageContent(request, eventSink, std::move(validated.inputs),
                               std::move(previousPersistence), plan, failure)) {
        result.diagnostics = std::move(failure.diagnostics);
        if (result.diagnostics.empty())
            result.diagnostics.push_back(
                {"PXPKG1300", "Package planning was cancelled"});
        return result;
    }
    std::vector<detail::PackageArchiveMount> mounts;
    for (std::uint32_t shard = 0; shard < request.archiveShards; ++shard) {
        if (std::ranges::any_of(plan.entries, [&](const ArchiveEntry& entry) {
                return ArchiveShardForUri(entry.uri, request.archiveShards) == shard;
            }))
            mounts.push_back({ShardArchiveName(shard, request.archiveShards),
                              "base", false});
    }
    result.manifestJson = detail::SerializePackageManifest(
        BuildPackageManifest(request, plan.packageFingerprint,
                             plan.encryptionKey, plan.entries,
                             std::move(mounts), std::move(plan.customEffects),
                             std::move(plan.legacyPersistenceReaders),
                             std::move(plan.compatibleSaveFingerprints)));
    if (const auto manifest = detail::ParsePackageManifest(result.manifestJson);
        !manifest.Valid()) {
        result.diagnostics = manifest.diagnostics;
        return result;
    }
    std::ostringstream contentIndex;
    if (!WriteContentIndex(contentIndex, request, plan.packageFingerprint,
                           plan.entries)) {
        result.diagnostics.push_back(
            {"PXPKG1311", "Package/content-index.json exceeds its size limit"});
        return result;
    }
    result.contentIndexJson = contentIndex.str();
    result.resources.reserve(plan.entries.size());
    for (auto& entry : plan.entries)
        result.resources.push_back({std::move(entry.uri), std::move(entry.source),
                                    std::move(entry.data), entry.size,
                                    std::move(entry.fingerprint)});
    result.allInputsVerified = depth == PackageValidationDepth::AllInputs;
    result.valid = true;
    return result;
}

PackageRunResult RunPackager(const PackageRequest& request, const PackageEventSink& eventSink) {
    EmitProgress(request, eventSink, "validate", 0, request.inputs.size(), "Validating package request");
    auto validated = Validate(request, eventSink,
                              PackageValidationDepth::Structural, true);
    if (validated.cancelled) {
        return Cancel(request, eventSink,
                      "Packaging was cancelled while validating inputs");
    }
    if (!validated.diagnostics.empty()) {
        return Fail(request, eventSink, std::move(validated.diagnostics));
    }
    if (Cancelled(request)) {
        return Cancel(request, eventSink, "Packaging was cancelled before staging");
    }

    // CLI packages into a fresh staging directory while the previous
    // distribution is supplied as its shard cache source.
    const auto persistenceSource = request.cacheSourceDir.empty()
        ? request.outputDir : request.cacheSourceDir;
    auto previousPersistence =
        DiscoverPreviousPersistence(persistenceSource, request.gameId);
    if (!previousPersistence.error.empty()) {
        return Fail(request, eventSink, "PXPKG1317",
                    previousPersistence.error, true);
    }

    std::error_code error;
    std::filesystem::create_directories(request.outputDir.parent_path(), error);
    if (error) {
        return Fail(request, eventSink, "PXPKG1301", "The output parent directory could not be created", true);
    }
    const auto staging = UniqueSibling(request.outputDir, "staging", request.requestId);
    const auto backup = UniqueSibling(request.outputDir, "backup", request.requestId);
    if (staging == request.outputDir || backup == request.outputDir || staging == backup) {
        return Fail(request, eventSink, "PXPKG1302", "Safe staging paths could not be created");
    }
    if (std::filesystem::exists(staging, error) || std::filesystem::exists(backup, error)) {
        return Fail(request, eventSink, "PXPKG1303", "A prior staging or backup directory still exists", true);
    }
    ScopedDirectory stagingCleanup{ staging };
    ScopedDirectory backupCleanup{ backup };
    std::filesystem::create_directories(staging, error);
    if (error) {
        return Fail(request, eventSink, "PXPKG1304", "The staging directory could not be created", true);
    }

    PackageContentPlan contentPlan;
    PackageRunResult planningFailure;
    if (!PreparePackageContent(request, eventSink, std::move(validated.inputs),
                               std::move(previousPersistence), contentPlan,
                               planningFailure))
        return planningFailure;
    auto& entries = contentPlan.entries;
    const auto& packageFingerprint = contentPlan.packageFingerprint;
    const auto& encryptionKey = contentPlan.encryptionKey;
    std::vector<std::vector<ArchiveEntry>> shards(request.archiveShards);
    for (const auto& entry : entries)
        shards[ArchiveShardForUri(entry.uri, request.archiveShards)].push_back(entry);
    const auto cacheSource = request.cacheSourceDir.empty()
                                 ? request.outputDir : request.cacheSourceDir;
    const bool cacheSourceWillBeRetired =
        cacheSource == request.outputDir ||
        request.retireCacheSourceAfterPromotion;
    const auto previousShardIndex = cacheSourceWillBeRetired
        ? LoadShardReuseIndex(cacheSource, request.archiveShards,
                              request.compression, request.encryption)
        : std::optional<ShardReuseIndex>{};
    std::vector<detail::PackageArchiveMount> archiveMounts;
    std::vector<std::pair<std::string, std::vector<CachedArchiveEntry>>> cacheDocuments;
    std::vector<std::pair<std::string, ShardReuseRecord>> shardRecords;
    std::size_t writtenEntries = 0;
    std::size_t reusedEntries = 0;
    std::size_t cacheEntryCount = 0;
    std::size_t reusedShards = 0;
    for (std::uint32_t index = 0; index < request.archiveShards; ++index) {
        const auto& shard = shards[index];
        if (shard.empty()) continue;
        const std::string archiveName = ShardArchiveName(index, request.archiveShards);
        const std::string cacheName = ShardCacheName(index, request.archiveShards);
        const auto contentFingerprint = ShardContentFingerprint(
            archiveName, shard, request.compression, request.encryption,
            encryptionKey);
        if (!contentFingerprint)
            return Fail(request, eventSink, "PXPKG1306",
                        "Archive shard fingerprint could not be computed");
        EmitProgress(request, eventSink, "archive", writtenEntries,
                     entries.size(), "Writing " + archiveName);
        const auto previousCache = !request.encryption &&
                                           CompressionLevel(request.compression) != 0
                                       ? LoadArchiveCache(cacheSource,
                                                          CompressionLevel(request.compression),
                                                          archiveName, cacheName)
                                       : std::optional<ArchiveCache>{};
        std::vector<CachedArchiveEntry> cacheEntries;
        std::size_t shardReused = 0;
        std::string archiveFingerprint;
        bool reusedWholeShard = false;
        const auto prior = previousShardIndex
                               ? previousShardIndex->find(archiveName)
                               : ShardReuseIndex::const_iterator{};
        if (previousShardIndex && prior != previousShardIndex->end() &&
            prior->second.contentFingerprint == *contentFingerprint) {
            const auto oldArchive = cacheSource / archiveName;
            error.clear();
            const auto status = std::filesystem::symlink_status(oldArchive, error);
            if (!error && std::filesystem::is_regular_file(status) &&
                ComputePackageFingerprint(oldArchive) ==
                    prior->second.archiveFingerprint) {
                // Validation happened earlier; a whole-shard reuse must still
                // recheck every current source at the writer's TOCTOU boundary.
                for (const auto& entry : shard) {
                    if (Cancelled(request))
                        return Cancel(request, eventSink,
                                      "Packaging was cancelled while verifying a shard");
                    if (entry.FileBacked() &&
                        ComputePackageFingerprint(entry.source) != entry.fingerprint)
                        return Fail(request, eventSink, "PXPKG1305",
                                    "An input changed while " + archiveName +
                                        " was being reused: " + entry.uri,
                                    true);
                }
                error.clear();
                std::filesystem::create_hard_link(
                    oldArchive, staging / archiveName, error);
                if (!error) {
                    reusedWholeShard = true;
                    archiveFingerprint = prior->second.archiveFingerprint;
                    ++reusedShards;
                    if (previousCache) {
                        for (const auto& [uri, cached] : previousCache->entries)
                            cacheEntries.push_back(cached);
                        std::ranges::sort(cacheEntries, {},
                                          &CachedArchiveEntry::uri);
                        shardReused = cacheEntries.size();
                    }
                    EmitProgress(request, eventSink, "archive-write",
                                 writtenEntries + shard.size(), entries.size(),
                                 "Reused unchanged " + archiveName);
                }
            }
        }
        if (!reusedWholeShard) {
            std::string archiveFailure;
            const auto archiveResult =
                WriteArchive(staging / archiveName, shard, request.compression,
                             request.encryption, DeriveKey(encryptionKey),
                             previousCache ? &*previousCache : nullptr,
                             cacheEntries, shardReused, archiveFailure,
                             [&](const std::size_t current, const std::size_t,
                                 const std::string& message) {
                                 EmitProgress(request, eventSink, "archive-write",
                                              writtenEntries + current,
                                              entries.size(), message);
                             });
            if (archiveResult == ArchiveWriteResult::InputChanged) {
                return Fail(request, eventSink, "PXPKG1305",
                            "An input changed while " + archiveName +
                                " was being written",
                            true);
            }
            if (archiveResult != ArchiveWriteResult::Completed) {
                return Fail(request, eventSink, "PXPKG1307",
                            archiveName + " could not be written" +
                                (archiveFailure.empty() ? std::string{} :
                                                          ": " + archiveFailure),
                            true);
            }
            archiveFingerprint = ComputePackageFingerprint(
                staging / archiveName);
            if (archiveFingerprint.empty())
                return Fail(request, eventSink, "PXPKG1307",
                            archiveName + " could not be fingerprinted", true);
        }
        archiveMounts.push_back({archiveName, "base", false});
        shardRecords.emplace_back(
            archiveName,
            ShardReuseRecord{*contentFingerprint, archiveFingerprint});
        writtenEntries += shard.size();
        reusedEntries += shardReused;
        cacheEntryCount += cacheEntries.size();
        cacheDocuments.emplace_back(cacheName, std::move(cacheEntries));
    }
    EmitProgress(request, eventSink, "archive-cache", reusedEntries,
                 cacheEntryCount, "Reused verified compressed entries");
    EmitProgress(request, eventSink, "archive-shard-reuse", reusedShards,
                 archiveMounts.size(), "Reused verified unchanged PDX shards");
    if (Cancelled(request)) {
        return Cancel(request, eventSink, "Packaging was cancelled after archive creation");
    }

    EmitProgress(request, eventSink, "runtime", 0, 1, "Copying Player and runtime libraries");
    const auto stagedPlayer = staging / request.playerExecutable.filename();
    std::filesystem::copy_file(request.playerExecutable, stagedPlayer, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return Fail(request, eventSink, "PXPKG1308", "Player executable could not be copied", true);
    }
    std::vector<std::filesystem::path> libraries;
    error.clear();
    for (std::filesystem::directory_iterator iterator(request.playerExecutable.parent_path(), error); !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        std::error_code entryError;
        if (iterator->is_regular_file(entryError) && RuntimeLibrary(iterator->path())) {
            libraries.push_back(iterator->path());
        }
    }
    if (error) {
        return Fail(request, eventSink, "PXPKG1309", "Adjacent runtime libraries could not be enumerated", true);
    }
    std::sort(libraries.begin(), libraries.end());
    for (const auto& library : libraries) {
        if (Cancelled(request)) {
            return Cancel(request, eventSink, "Packaging was cancelled while copying runtime libraries");
        }
        error.clear();
        std::filesystem::copy_file(library, staging / library.filename(), std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return Fail(request, eventSink, "PXPKG1310", "Runtime library could not be copied: " + px::PathToUtf8(library.filename()), true);
        }
    }

    // Installed runtime bundles carry a versioned notice inventory. Keep the
    // Packager usable from a contributor build tree (where the share directory
    // is absent), but copy the complete inventory into every retail package
    // created from an installed runtime.
    const auto runtimeShare = request.playerExecutable.parent_path().parent_path() /
                              "share/PrismatiX";
    error.clear();
    const bool runtimeShareExists = std::filesystem::exists(runtimeShare, error);
    if (error) {
        return Fail(request, eventSink, "PXPKG1319",
                    "Runtime release inventory could not be inspected", true);
    }
    if (runtimeShareExists &&
        std::filesystem::is_directory(runtimeShare, error)) {
        const auto notices = staging / "Notices";
        std::filesystem::create_directories(notices, error);
        if (error) {
            return Fail(request, eventSink, "PXPKG1319",
                        "Release notice directory could not be created", true);
        }
        for (const auto& name : {"LICENSE", "THIRD_PARTY_NOTICES.md",
                                 "ASSET_PROVENANCE.json",
                                 "release-inventory.json"}) {
            const auto source = runtimeShare / name;
            error.clear();
            if (std::filesystem::is_regular_file(source, error)) {
                std::filesystem::copy_file(
                    source, notices / name,
                    std::filesystem::copy_options::overwrite_existing, error);
                if (error) {
                    return Fail(request, eventSink, "PXPKG1319",
                                std::string("Release notice could not be copied: ") +
                                    name,
                                true);
                }
            }
        }
        for (const auto& directory : {"licenses", "docs/releases"}) {
            const auto source = runtimeShare / directory;
            error.clear();
            if (std::filesystem::is_directory(source, error)) {
                const auto destination = notices / directory;
                std::filesystem::create_directories(destination.parent_path(),
                                                    error);
                if (error) {
                    return Fail(request, eventSink, "PXPKG1319",
                                std::string("Release inventory directory could not be created: ") +
                                    directory,
                                true);
                }
                std::filesystem::copy(
                    source, destination,
                    std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing,
                    error);
                if (error) {
                    return Fail(request, eventSink, "PXPKG1319",
                                std::string("Release inventory could not be copied: ") +
                                    directory,
                                true);
                }
            }
        }
    } else if (error || runtimeShareExists) {
        return Fail(request, eventSink, "PXPKG1319",
                    "Runtime release inventory could not be inspected", true);
    }

    const std::string manifest = detail::SerializePackageManifest(
        BuildPackageManifest(request, packageFingerprint, encryptionKey, entries,
                             std::move(archiveMounts),
                             std::move(contentPlan.customEffects),
                             std::move(contentPlan.legacyPersistenceReaders),
                             std::move(contentPlan.compatibleSaveFingerprints)));
    const auto manifestSelfCheck = detail::ParsePackageManifest(manifest);
    if (!manifestSelfCheck.Valid()) {
        const auto& diagnostic = manifestSelfCheck.diagnostics.front();
        return Fail(request, eventSink, diagnostic.code, diagnostic.message);
    }
    {
        std::filesystem::create_directories(staging / "Package", error);
        if (error) {
            return Fail(request, eventSink, "PXPKG1311",
                        "Package manifest directory could not be created", true);
        }
        if (!request.encryption && CompressionLevel(request.compression) != 0) {
            for (const auto& [cacheName, cacheEntries] : cacheDocuments) {
                Json cacheDocument{{"format", "PrismatiXPackageEntryCache"},
                                   {"schemaRevision", 1},
                                   {"compressionLevel", CompressionLevel(request.compression)},
                                   {"entries", Json::array()}};
                for (const auto& entry : cacheEntries) {
                    cacheDocument["entries"].push_back({
                        {"uri", entry.uri}, {"fingerprint", entry.fingerprint},
                        {"offset", entry.offset}, {"rawSize", entry.rawSize},
                        {"storedSize", entry.storedSize}, {"crc", entry.crc}});
                }
                const std::string cacheText = cacheDocument.dump();
                if (cacheText.size() <= 64ull * 1024ull * 1024ull) {
                    std::ofstream cacheOutput(staging / "Package" / cacheName,
                                              std::ios::binary | std::ios::trunc);
                    cacheOutput << cacheText;
                    if (!cacheOutput) {
                        return Fail(request, eventSink, "PXPKG1311",
                                    "Package/" + cacheName + " could not be written", true);
                    }
                }
            }
        }
        Json shardIndex{{"format", "PrismatiXArchiveShardIndex"},
                        {"schemaRevision", 1},
                        {"archiveShards", request.archiveShards},
                        {"compressionLevel", CompressionLevel(request.compression)},
                        {"encrypted", request.encryption},
                        {"archives", Json::array()}};
        for (const auto& [file, record] : shardRecords)
            shardIndex["archives"].push_back(
                {{"file", file},
                 {"contentFingerprint", record.contentFingerprint},
                 {"archiveFingerprint", record.archiveFingerprint}});
        const std::string shardIndexText = shardIndex.dump();
        if (shardIndexText.size() > 64ull * 1024ull)
            return Fail(request, eventSink, "PXPKG1311",
                        "Package/archive-shards.json exceeds its size limit");
        std::ofstream shardIndexOutput(
            staging / "Package/archive-shards.json",
            std::ios::binary | std::ios::trunc);
        shardIndexOutput << shardIndexText;
        if (!shardIndexOutput)
            return Fail(request, eventSink, "PXPKG1311",
                        "Package/archive-shards.json could not be written", true);
        std::ofstream contentIndexOutput(
            staging / "Package/content-index.json",
            std::ios::binary | std::ios::trunc);
        if (!WriteContentIndex(contentIndexOutput, request,
                               packageFingerprint, entries))
            return Fail(request, eventSink, "PXPKG1311",
                        "Package/content-index.json exceeds its size limit or could not be written",
                        true);
        std::ofstream output(staging / "Package/manifest.json", std::ios::binary | std::ios::trunc);
        output << manifest;
        if (!output) {
            return Fail(request, eventSink, "PXPKG1311", "Package/manifest.json could not be written", true);
        }
    }
    if (Cancelled(request)) {
        return Cancel(request, eventSink, "Packaging was cancelled before output promotion");
    }

    EmitProgress(request, eventSink, "promote", 0, 1, "Promoting staged output");
    const bool previousOutput = std::filesystem::exists(request.outputDir, error);
    if (error) {
        return Fail(request, eventSink, "PXPKG1312", "The prior output could not be inspected", true);
    }
    if (previousOutput) {
        std::filesystem::rename(request.outputDir, backup, error);
        if (error) {
            return Fail(request, eventSink, "PXPKG1313", "The prior successful output could not be preserved", true);
        }
    }
    if (Cancelled(request)) {
        if (previousOutput) {
            error.clear();
            std::filesystem::rename(backup, request.outputDir, error);
            if (!error) backupCleanup.keep = true;
        }
        return error ? Fail(request, eventSink, "PXPKG1314", "Cancellation occurred and prior output restoration failed", true) : Cancel(request, eventSink, "Packaging was cancelled before output promotion");
    }
    error.clear();
    std::filesystem::rename(staging, request.outputDir, error);
    if (error) {
        if (previousOutput) {
            std::error_code restoreError;
            std::filesystem::rename(backup, request.outputDir, restoreError);
            if (!restoreError) backupCleanup.keep = true;
        }
        return Fail(request, eventSink, "PXPKG1315", "Staged output promotion failed; prior output was restored", true);
    }
    if (previousOutput) {
        const auto previousSave = backup / "Save";
        error.clear();
        const bool hasPreviousSave =
            std::filesystem::exists(previousSave, error);
        if (!error && hasPreviousSave) {
            std::filesystem::rename(previousSave,
                                    request.outputDir / "Save", error);
        }
        if (error) {
            std::error_code discardError;
            std::filesystem::rename(request.outputDir, staging, discardError);
            std::error_code restoreError;
            if (!discardError)
                std::filesystem::rename(backup, request.outputDir,
                                        restoreError);
            if (discardError || restoreError) {
                backupCleanup.keep = true;
                stagingCleanup.keep = true;
                return Fail(
                    request, eventSink, "PXPKG1318",
                    "User save preservation failed and the prior output could not be restored; both copies were retained",
                    true);
            }
            backupCleanup.keep = true;
            return Fail(request, eventSink, "PXPKG1318",
                        "User save preservation failed; the prior output was restored",
                        true);
        }
    }
    stagingCleanup.keep = true;
    if (previousOutput) {
        error.clear();
        std::filesystem::remove_all(backup, error);
        if (error) {
            return Fail(request, eventSink, "PXPKG1316", "The obsolete output backup could not be removed", true);
        }
        backupCleanup.keep = true;
    }

    PackageEvent completed;
    completed.kind = PackageEventKind::Completed;
    completed.requestId = request.requestId;
    completed.outputDir = request.outputDir;
    completed.playerExecutable = request.outputDir / request.playerExecutable.filename();
    completed.manifestPath = request.outputDir / "Package/manifest.json";
    completed.inputCount = request.inputs.size();
    Emit(eventSink, std::move(completed));
    PackageRunResult result;
    result.exitCode = PackageExitCode::Completed;
    return result;
}

}  // namespace px::sdk
