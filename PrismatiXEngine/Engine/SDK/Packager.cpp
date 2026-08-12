#include "Engine/SDK/Packager.h"

#include <psa/crypto.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/StudioUi.h"
#include "Engine/SDK/CharacterResources.h"
#include "Engine/SDK/GameCatalogResources.h"

namespace px::sdk {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;
using Key = std::array<std::uint8_t, 32>;
using Iv = std::array<std::uint8_t, 16>;

constexpr char kArchiveMagic[4] = { 'P', 'D', 'X', '4' };
constexpr std::uint32_t kArchiveVersion = 4;
constexpr std::size_t kArchiveHeaderSize = 28;
constexpr std::uint8_t kEntryEncrypted = 0x01;
constexpr std::uint8_t kEntryCompressed = 0x02;
constexpr std::uint32_t kArchiveEncrypted = 0x01;
constexpr char kIndexSalt[] = "__pdx4_index__";
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
    Bytes data;
};

struct ValidationResult {
    std::vector<ValidatedInput> inputs;
    std::vector<PackageDiagnostic> diagnostics;
};

std::filesystem::path Utf8Path(const std::string_view value) { return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size())); }

void AddDiagnostic(std::vector<PackageDiagnostic>& diagnostics, std::string code, std::string message, const bool retryable = false) { diagnostics.push_back({ std::move(code), std::move(message), retryable }); }

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

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsFingerprint(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const unsigned char c) { return std::isxdigit(c) != 0; });
}

bool IsSafeUri(const std::string_view uri) {
    if (uri.empty() || uri.size() > 4096 || uri.front() == '/' || uri.find('\\') != std::string_view::npos || uri.find(':') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path = Utf8Path(uri);
    if (path.is_absolute() || path.generic_string() != uri) return false;
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") return false;
    }
    return path.lexically_normal().generic_string() == uri;
}

std::filesystem::path WeakCanonical(const std::filesystem::path& path, std::error_code& error) {
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) return {};
    return result.lexically_normal();
}

bool EqualComponent(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    return Lower(left.string()) == Lower(right.string());
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

std::optional<Bytes> Encrypt(const Bytes& input, const Key& key, const Iv& iv) {
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
    const psa_status_t encrypted = psa_aead_encrypt(keyId, PSA_ALG_GCM, output.data(), nonceSize, nullptr, 0, input.data(), input.size(), output.data() + nonceSize, output.size() - nonceSize, &written);
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

std::uint32_t Crc32(const Bytes& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
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

std::optional<Bytes> Compress(const Bytes& source, const int level) {
    if (level == 0 || source.empty()) return source;
    const std::size_t bound = ZSTD_compressBound(source.size());
    Bytes result(bound);
    const std::size_t size = ZSTD_compress(result.data(), result.size(), source.data(), source.size(), level);
    if (ZSTD_isError(size) != 0) return std::nullopt;
    result.resize(size);
    return result;
}

bool WriteBytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(output);
}

bool WriteArchive(const std::filesystem::path& path, const std::vector<ArchiveEntry>& entries, const PackageCompression compression, const bool encryption, const Key& key) {
    Bytes data;
    Bytes index;
    PutU32(index, static_cast<std::uint32_t>(entries.size()));
    std::unordered_set<std::uint64_t> hashes;
    const int compressionLevel = CompressionLevel(compression);

    for (const auto& entry : entries) {
        if (!IsSafeUri(entry.uri) || entry.uri.size() > static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()) || entry.data.size() > kMaxEntrySize) {
            return false;
        }
        const std::uint64_t pathHash = HashPath(entry.uri);
        if (!hashes.insert(pathHash).second) return false;

        std::uint8_t flags = 0;
        Bytes stored = entry.data;
        if (compressionLevel != 0 && !entry.data.empty()) {
            auto compressed = Compress(entry.data, compressionLevel);
            if (!compressed) return false;
            if (compressed->size() < stored.size()) {
                stored = std::move(*compressed);
                flags |= kEntryCompressed;
            }
        }
        if (encryption) {
            auto encrypted = Encrypt(stored, key, DeriveIv(entry.uri));
            if (!encrypted) return false;
            stored = std::move(*encrypted);
            flags |= kEntryEncrypted;
        }

        const std::uint64_t offset = kArchiveHeaderSize + data.size();
        data.insert(data.end(), stored.begin(), stored.end());
        PutU64(index, pathHash);
        PutU16(index, static_cast<std::uint16_t>(entry.uri.size()));
        index.insert(index.end(), entry.uri.begin(), entry.uri.end());
        PutU64(index, offset);
        PutU64(index, entry.data.size());
        PutU64(index, stored.size());
        PutU32(index, Crc32(entry.data));
        index.push_back(flags);
    }

    if (encryption) {
        auto encrypted = Encrypt(index, key, DeriveIv(kIndexSalt));
        if (!encrypted) return false;
        index = std::move(*encrypted);
    }
    if (index.empty()) return false;

    Bytes output;
    output.reserve(kArchiveHeaderSize + data.size() + index.size());
    output.insert(output.end(), kArchiveMagic, kArchiveMagic + 4);
    PutU32(output, kArchiveVersion);
    PutU32(output, encryption ? kArchiveEncrypted : 0u);
    PutU64(output, kArchiveHeaderSize + data.size());
    PutU64(output, index.size());
    output.insert(output.end(), data.begin(), data.end());
    output.insert(output.end(), index.begin(), index.end());
    return WriteBytes(path, output);
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
    const auto studio = ParseStudioUi(text);
    return studio.Valid() && IsUuid(studio.document.id)
               ? std::optional<std::string>{studio.document.id}
               : std::nullopt;
}

std::string Quote(const std::string_view value) {
    std::string result{ "\"" };
    for (const char character : value) {
        switch (character) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result.push_back(character);
                break;
        }
    }
    result.push_back('"');
    return result;
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

std::string CompressionText(const PackageCompression value) {
    switch (value) {
        case PackageCompression::None:
            return "none";
        case PackageCompression::Fast:
            return "fast";
        case PackageCompression::Balanced:
            return "balanced";
        case PackageCompression::Maximum:
            return "maximum";
    }
    return "none";
}

std::string PlatformText() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string PackageManifest(const PackageRequest& request, const std::string& packageFingerprint, const std::string& encryptionKey, const std::vector<ArchiveEntry>& entries) {
    auto routesInOrder = request.routes;
    std::sort(routesInOrder.begin(), routesInOrder.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    std::string routes = "array(";
    for (std::size_t index = 0; index < routesInOrder.size(); ++index) {
        if (index != 0) routes += ", ";
        const auto archived = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.uri == routesInOrder[index].scene; });
        const auto sceneText = archived == entries.end() ? std::optional<std::string>{} : std::optional<std::string>(std::string(archived->data.begin(), archived->data.end()));
        const auto sceneId = sceneText ? PlayerUiSceneId(*sceneText) : std::optional<std::string>{};
        routes += "object(\"id\", " + Quote(routesInOrder[index].id) + ", \"scene\", res(" + Quote(sceneId.value_or(UuidFromFingerprint(packageFingerprint))) + ", " + Quote(routesInOrder[index].scene) + "), \"modal\", false, \"cache\", true)";
    }
    routes += ")";

    std::ostringstream output;
    output << "@pxresource 4 " << UuidFromFingerprint(packageFingerprint) << " GamePackage\n\n";
    output << "archives = array(object(\"file\", \"Content.pdx\", \"group\", "
              "\"base\", \"optional\", false))\n";
    output << "compression = " << Quote(CompressionText(request.compression)) << '\n';
    output << "contentVersion = " << Quote(request.contentVersion) << '\n';
    output << "encrypt = " << (request.encryption ? "true" : "false") << '\n';
    output << "gameHeight = " << request.height << '\n';
    output << "gameWidth = " << request.width << '\n';
    output << "key = " << Quote(request.encryption ? encryptionKey : std::string{}) << '\n';
    output << "platform = " << Quote(PlatformText()) << '\n';
    output << "productVersion = " << Quote(request.contentVersion) << '\n';
    output << "profile = \"studio\"\n";
    output << "reproducible = true\n";
    output << "routes = " << routes << '\n';
    output << "splashes = array()\n";
    output << "startRoute = " << Quote(request.startRoute) << '\n';
    output << "startScript = " << Quote(request.startScript) << '\n';
    output << "title = " << Quote(request.title) << '\n';
    return output.str();
}

bool RuntimeLibrary(const std::filesystem::path& path) {
    const std::string extension = Lower(path.extension().string());
#ifdef _WIN32
    return extension == ".dll";
#elif defined(__APPLE__)
    return extension == ".dylib";
#else
    return extension == ".so" || path.filename().string().find(".so.") != std::string::npos;
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
    PackageEvent event;
    event.kind = PackageEventKind::Failed;
    event.requestId = request.requestId;
    event.code = code;
    event.message = message;
    event.retryable = retryable;
    Emit(sink, std::move(event));
    PackageRunResult result;
    result.exitCode = PackageExitCode::Failed;
    result.diagnostics.push_back({ std::move(code), std::move(message), retryable });
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

ValidationResult Validate(const PackageRequest& request) {
    ValidationResult result;
    auto& diagnostics = result.diagnostics;
    if (!IsRequestId(request.requestId)) {
        AddDiagnostic(diagnostics, "PXPKG1201", "requestId contains unsupported characters");
    }
    if (!request.projectRoot.is_absolute() || !request.outputDir.is_absolute() || !request.playerExecutable.is_absolute() || !request.cancelFile.is_absolute()) {
        AddDiagnostic(
            diagnostics,
            "PXPKG1202",
            "projectRoot, outputDir, playerExecutable, and cancelFile "
            "must be absolute"
        );
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
    if (request.contentVersion.empty() || request.contentVersion.size() > 128) {
        AddDiagnostic(diagnostics, "PXPKG1206", "contentVersion is invalid");
    }
    if (request.inputs.empty() || request.inputs.size() > kMaxInputs) {
        AddDiagnostic(diagnostics, "PXPKG1207", "inputs count is invalid");
    }

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
    for (const auto& input : request.inputs) {
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
        const std::string actualFingerprint = ComputePackageFingerprint(source);
        if (actualFingerprint.empty() || actualFingerprint != Lower(input.fingerprint)) {
            AddDiagnostic(diagnostics, "PXPKG1216", "input fingerprint changed: " + input.uri, true);
            continue;
        }
        result.inputs.push_back({ input, source });
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
            ReadTextFile(projectInput->source, projectInput->input.size);
        std::unordered_map<std::string, const ValidatedInput*> packagedInputs;
        for (const auto& input : result.inputs)
            packagedInputs.emplace(input.input.uri, &input);
        const auto readPackaged =
            [&packagedInputs](const std::string_view uri)
                -> std::optional<std::string> {
            const auto found = packagedInputs.find(std::string(uri));
            if (found == packagedInputs.end()) return std::nullopt;
            return ReadTextFile(found->second->source,
                                found->second->input.size);
        };
        const auto packagedExists =
            [&packagedInputs](const std::string_view uri) {
                return packagedInputs.contains(std::string(uri));
            };
        if (!projectText) {
            AddDiagnostic(diagnostics, "PXPKG1227",
                          "project.pxproject could not be read", true);
        } else {
            const auto characters = LoadCharacterResources(
                *projectText, readPackaged, packagedExists);
            for (const auto& diagnostic : characters.diagnostics) {
                AddDiagnostic(
                    diagnostics, diagnostic.code,
                    diagnostic.message +
                        (diagnostic.path.empty()
                             ? std::string{}
                             : std::string(" [") + diagnostic.path + "]"));
            }
            if (characters.diagnostics.empty()) {
                const auto catalog =
                    packagedInputs.find("Content/Game.pxres");
                if (catalog == packagedInputs.end()) {
                    if (!characters.declared) {
                        AddDiagnostic(
                            diagnostics, "PXPKG1228",
                            "legacy project without characterResources requires Content/Game.pxres");
                    }
                } else {
                    const auto catalogText = ReadTextFile(
                        catalog->second->source, catalog->second->input.size);
                    if (!catalogText) {
                        AddDiagnostic(
                            diagnostics, "PXPKG1229",
                            "Content/Game.pxres could not be read", true);
                    } else {
                        const auto runtimeCatalog = LoadGameCatalogResources(
                            *catalogText, "Content/Game.pxres",
                            characters.declared
                                ? LegacyGameCatalogPolicy::RejectCharacterNodes
                                : LegacyGameCatalogPolicy::AllowCharacterNodes,
                            characters.declared
                                ? LegacyGalleryReferencePolicy::RejectPathStrings
                                : LegacyGalleryReferencePolicy::AllowPathStrings);
                        for (const auto& diagnostic :
                             runtimeCatalog.diagnostics) {
                            AddDiagnostic(
                                diagnostics, diagnostic.code,
                                diagnostic.message +
                                    (diagnostic.nodeId.empty()
                                         ? std::string{}
                                         : std::string(" [node ") +
                                               diagnostic.nodeId + "]"));
                        }
                        if (runtimeCatalog.Valid() && characters.declared) {
                            const auto resolvedCatalog =
                                ResolveGameCatalogGalleryResources(
                                    runtimeCatalog.document, *projectText,
                                    packagedExists, "Content/Game.pxres");
                            for (const auto& diagnostic :
                                 resolvedCatalog.diagnostics) {
                                AddDiagnostic(
                                    diagnostics, diagnostic.code,
                                    diagnostic.message +
                                        (diagnostic.nodeId.empty()
                                             ? std::string{}
                                             : std::string(" [node ") +
                                                   diagnostic.nodeId + "]"));
                            }
                        }
                    }
                }
            }
        }
    }
    if (!uris.contains(request.startScript)) {
        AddDiagnostic(diagnostics, "PXPKG1217", "startScript must be present in inputs");
    }
    else {
        const auto script = std::find_if(result.inputs.begin(), result.inputs.end(), [&](const auto& input) { return input.input.uri == request.startScript; });
        if (script != result.inputs.end()) {
            const auto text = ReadTextFile(script->source, script->input.size);
            if (!text || !ParseRuntimeIr(*text).Valid()) {
                AddDiagnostic(diagnostics, "PXPKG1223", "startScript is not valid compiled Runtime IR");
            }
        }
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
                const auto text = ReadTextFile(scene->source, scene->input.size);
                if (!text || !PlayerUiSceneId(*text)) {
                    AddDiagnostic(diagnostics, "PXPKG1224", "route scene is neither a typed UIScene nor Studio UI: " + route.scene);
                }
            }
        }
    }
    if (!request.startRoute.empty() && !routeIds.contains(request.startRoute)) {
        AddDiagnostic(diagnostics, "PXPKG1222", "startRoute must identify one of the packaged routes");
    }
    return result;
}

std::filesystem::path UniqueSibling(const std::filesystem::path& output, const std::string_view label, const std::string_view requestId) {
    const auto digest = Sha256Text(std::string(requestId) + "|" + output.generic_string() + "|" + std::string(label));
    const std::string suffix = digest ? Hex(*digest).substr(0, 16) : std::string("0000000000000000");
    return output.parent_path() / (output.filename().string() + "." + std::string(label) + "-" + suffix);
}

}  // namespace

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
    if (!revisionValue || *revisionValue != 1) {
        AddDiagnostic(result.diagnostics, "PXPKG1103", "schemaRevision must be 1");
    }
    RequiredString(root, "requestId", result.request.requestId, result.diagnostics);
    result.request.projectRoot = JsonPath(root, "projectRoot");
    result.request.outputDir = JsonPath(root, "outputDir");
    result.request.playerExecutable = JsonPath(root, "playerExecutable");
    result.request.cancelFile = JsonPath(root, "cancelFile");
    RequiredString(root, "title", result.request.title, result.diagnostics);
    RequiredString(root, "startScript", result.request.startScript, result.diagnostics);
    RequiredString(root, "contentVersion", result.request.contentVersion, result.diagnostics);

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
            object["outputDir"] = event.outputDir.generic_string();
            object["playerExecutable"] = event.playerExecutable.generic_string();
            object["manifestPath"] = event.manifestPath.generic_string();
            object["inputCount"] = event.inputCount;
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

PackageRunResult RunPackager(const PackageRequest& request, const PackageEventSink& eventSink) {
    EmitProgress(request, eventSink, "validate", 0, request.inputs.size(), "Validating package request");
    auto validated = Validate(request);
    if (!validated.diagnostics.empty()) {
        const auto& first = validated.diagnostics.front();
        return Fail(request, eventSink, first.code, first.message, first.retryable);
    }
    if (Cancelled(request)) {
        return Cancel(request, eventSink, "Packaging was cancelled before staging");
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

    std::sort(validated.inputs.begin(), validated.inputs.end(), [](const auto& left, const auto& right) { return left.input.uri < right.input.uri; });
    std::vector<ArchiveEntry> entries;
    entries.reserve(validated.inputs.size());
    std::string identityMaterial = request.contentVersion;
    for (std::size_t index = 0; index < validated.inputs.size(); ++index) {
        if (Cancelled(request)) {
            return Cancel(request, eventSink, "Packaging was cancelled while reading inputs");
        }
        const auto& input = validated.inputs[index];
        EmitProgress(request, eventSink, "archive", index, validated.inputs.size(), "Reading " + input.input.uri);
        auto bytes = ReadFile(input.source, input.input.size);
        if (!bytes) {
            return Fail(request, eventSink, "PXPKG1305", "An input changed while packaging: " + input.input.uri, true);
        }
        const auto readFingerprint = Sha256Bytes(bytes->data(), bytes->size());
        if (!readFingerprint || Hex(*readFingerprint) != Lower(input.input.fingerprint)) {
            return Fail(request, eventSink, "PXPKG1305", "An input changed while packaging: " + input.input.uri, true);
        }
        entries.push_back({ input.input.uri, std::move(*bytes) });
        identityMaterial += "|" + input.input.uri + "|" + Lower(input.input.fingerprint);
    }
    const auto packageDigest = Sha256Text(identityMaterial);
    if (!packageDigest) {
        return Fail(request, eventSink, "PXPKG1306", "SHA-256 is unavailable");
    }
    const std::string packageFingerprint = Hex(*packageDigest);
    const std::string encryptionKey = "pxpkg-" + Hex(DeriveKey("PrismatiXPackage|" + identityMaterial));
    EmitProgress(request, eventSink, "archive", validated.inputs.size(), validated.inputs.size(), "Writing Content.pdx");
    if (!WriteArchive(staging / "Content.pdx", entries, request.compression, request.encryption, DeriveKey(encryptionKey))) {
        return Fail(request, eventSink, "PXPKG1307", "Content.pdx could not be written", true);
    }
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
            return Fail(request, eventSink, "PXPKG1310", "Runtime library could not be copied: " + library.filename().string(), true);
        }
    }

    const std::string manifest = PackageManifest(request, packageFingerprint, encryptionKey, entries);
    {
        std::ofstream output(staging / "game.pxpackage", std::ios::binary | std::ios::trunc);
        output << manifest;
        if (!output) {
            return Fail(request, eventSink, "PXPKG1311", "game.pxpackage could not be written", true);
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
    completed.manifestPath = request.outputDir / "game.pxpackage";
    completed.inputCount = request.inputs.size();
    Emit(eventSink, std::move(completed));
    PackageRunResult result;
    result.exitCode = PackageExitCode::Completed;
    return result;
}

}  // namespace px::sdk
