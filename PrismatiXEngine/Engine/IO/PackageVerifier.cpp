#include "Engine/IO/PackageVerifier.h"

#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/OverlaySignature.h"
#include "Engine/IO/VFS.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Support/Utf8Path.h"

#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace px::io {
namespace {

using Json = nlohmann::json;

struct IndexedResource {
    std::string uri;
    std::string fingerprint;
    std::uint64_t size = 0;
    std::string archive;
};

struct BaseArchiveStamp {
    std::string name;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified;
    bool operator==(const BaseArchiveStamp&) const = default;
};

#ifdef _WIN32
struct BaseArchiveReadLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit BaseArchiveReadLock(HANDLE value) : handle(value) {}
    BaseArchiveReadLock(const BaseArchiveReadLock&) = delete;
    BaseArchiveReadLock& operator=(const BaseArchiveReadLock&) = delete;
    BaseArchiveReadLock(BaseArchiveReadLock&& other) noexcept
        : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
    BaseArchiveReadLock& operator=(BaseArchiveReadLock&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            handle = std::exchange(other.handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~BaseArchiveReadLock() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
#endif

struct PreverifiedOverlayBase {
    std::filesystem::path directory;
    std::string manifest;
    std::string index;
    std::vector<BaseArchiveStamp> archives;
    PackageContentVerification contents;
    std::string error;
#ifdef _WIN32
    std::vector<BaseArchiveReadLock> readLocks;
#endif
};

PackageContentVerification Fail(std::string message) {
    return {.error = std::move(message)};
}

std::optional<std::string> ReadBoundedRegularFile(
    const std::filesystem::path& path, const std::uintmax_t limit) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) return std::nullopt;
    const auto length = std::filesystem::file_size(path, error);
    if (error || length == 0 || length > limit ||
        length > static_cast<std::uintmax_t>(
            (std::numeric_limits<std::streamsize>::max)())) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string value(static_cast<std::size_t>(length), '\0');
    if (!input.read(value.data(), static_cast<std::streamsize>(length)))
        return std::nullopt;
    return value;
}

std::optional<std::vector<BaseArchiveStamp>> BaseArchiveStamps(
    const std::filesystem::path& directory,
    const std::string_view manifestText) {
    const auto parsed = sdk::detail::ParsePackageManifest(manifestText);
    if (!parsed.Valid()) return std::nullopt;
    std::vector<BaseArchiveStamp> result;
    result.reserve(parsed.manifest.archives.size());
    for (const auto& mount : parsed.manifest.archives) {
        const auto path = directory / px::PathFromUtf8(mount.file);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status)) return std::nullopt;
        const auto size = std::filesystem::file_size(path, error);
        if (error) return std::nullopt;
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) return std::nullopt;
        result.push_back({mount.file, size, modified});
    }
    return result;
}

#ifdef _WIN32
std::optional<std::vector<BaseArchiveReadLock>> LockBaseArchives(
    const std::filesystem::path& directory,
    const std::string_view manifestText) {
    const auto parsed = sdk::detail::ParsePackageManifest(manifestText);
    if (!parsed.Valid()) return std::nullopt;
    std::vector<BaseArchiveReadLock> locks;
    locks.reserve(parsed.manifest.archives.size());
    for (const auto& mount : parsed.manifest.archives) {
        const auto path = directory / px::PathFromUtf8(mount.file);
        const auto handle = CreateFileW(
            path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
        locks.emplace_back(handle);
    }
    return locks;
}
#endif

bool LowerFingerprint(const std::string_view value) {
    return value.size() == 64 &&
        std::ranges::all_of(value, [](const unsigned char byte) {
            return std::isdigit(byte) || (byte >= 'a' && byte <= 'f');
        });
}

bool ValidOptionalPackId(const std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
        std::ranges::all_of(value, [](const unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                   (byte >= '0' && byte <= '9') || byte == '-';
        });
}

unsigned VerificationWorkers(const std::size_t archives, const bool encrypted) {
    // Legacy encrypted+compressed entries still use a whole-entry buffer.
    // Never multiply that allocation across archive workers.
    if (encrypted) return archives == 0 ? 0 : 1;
    unsigned workers = std::min<unsigned>(4, std::thread::hardware_concurrency());
    if (workers == 0) workers = 1;
#ifdef _WIN32
    char* configuredStorage = nullptr;
    std::size_t configuredSize = 0;
    _dupenv_s(&configuredStorage, &configuredSize,
              "PRISMATIX_PACKAGE_VERIFY_WORKERS");
    const char* configured = configuredStorage;
#else
    const char* configured = std::getenv("PRISMATIX_PACKAGE_VERIFY_WORKERS");
#endif
    if (configured) {
        unsigned requested = 0;
        const auto end = configured + std::strlen(configured);
        const auto parsed = std::from_chars(configured, end, requested);
        if (parsed.ec == std::errc{} && parsed.ptr == end &&
            requested >= 1 && requested <= 8)
            workers = requested;
    }
#ifdef _WIN32
    std::free(configuredStorage);
#endif
    return static_cast<unsigned>(std::min<std::size_t>(workers, archives));
}

std::optional<std::string> FingerprintStream(
    SeekableReadStream& stream,
    const std::optional<std::uint32_t> expectedCrc = std::nullopt) {
    if (psa_crypto_init() != PSA_SUCCESS) return std::nullopt;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return std::nullopt;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t remaining = stream.Size();
    std::uint32_t crc = 0xffffffffu;
    while (remaining > 0) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        const auto received = stream.Read(buffer.data(), requested);
        if (received == 0 || received > requested || stream.Failed() ||
            psa_hash_update(&operation, buffer.data(), received) != PSA_SUCCESS) {
            psa_hash_abort(&operation);
            return std::nullopt;
        }
        if (expectedCrc)
            crc = crypto::UpdateCrc32(crc, buffer.data(), received);
        remaining -= received;
    }
    if (stream.Failed() || (expectedCrc && ~crc != *expectedCrc)) {
        psa_hash_abort(&operation);
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> digest{};
    std::size_t length = 0;
    if (psa_hash_finish(&operation, digest.data(), digest.size(), &length) !=
            PSA_SUCCESS || length != digest.size()) {
        psa_hash_abort(&operation);
        return std::nullopt;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        value.push_back(hex[byte >> 4]);
        value.push_back(hex[byte & 15]);
    }
    return value;
}

std::optional<std::string> FingerprintText(const std::string_view value) {
    if (psa_crypto_init() != PSA_SUCCESS) return std::nullopt;
    std::array<std::uint8_t, 32> digest{};
    std::size_t size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256,
                         reinterpret_cast<const std::uint8_t*>(value.data()),
                         value.size(), digest.data(), digest.size(), &size) !=
            PSA_SUCCESS || size != digest.size()) return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    std::string fingerprint;
    fingerprint.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        fingerprint.push_back(hex[byte >> 4]);
        fingerprint.push_back(hex[byte & 15]);
    }
    return fingerprint;
}

std::optional<std::string> ReadBoundedArchiveText(
    const Archive& archive, const std::string_view name,
    const std::uint64_t limit) {
    const auto found = std::ranges::find_if(
        archive.Entries(), [name](const Archive::Entry& entry) {
            return entry.name == name;
        });
    if (found == archive.Entries().end() || found->rawSize == 0 ||
        found->rawSize > limit) return std::nullopt;
    const auto bytes = archive.Read(name);
    if (!bytes || bytes->size() != found->rawSize) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

std::optional<std::map<std::string, IndexedResource>> ParseIndexedResources(
    const std::string_view text, const sdk::detail::PackageManifest& manifest) {
    const Json index = Json::parse(text, nullptr, false);
    if (!index.is_object() || !index.contains("format") ||
        !index["format"].is_string() ||
        index["format"] != "PrismatiXContentIndex" ||
        !index.contains("schemaRevision") ||
        !index["schemaRevision"].is_number_integer() ||
        index["schemaRevision"] != 1 ||
        !index.contains("gameId") || !index["gameId"].is_string() ||
        index["gameId"] != manifest.gameId ||
        !index.contains("packageFingerprint") ||
        !index["packageFingerprint"].is_string() ||
        index["packageFingerprint"] != manifest.packageFingerprint ||
        !index.contains("entries") || !index["entries"].is_array() ||
        index["entries"].empty() || index["entries"].size() > 1'000'000)
        return std::nullopt;
    std::set<std::string> archives;
    for (const auto& mount : manifest.archives) archives.insert(mount.file);
    std::map<std::string, IndexedResource> entries;
    std::string previous;
    for (const auto& value : index["entries"]) {
        if (!value.is_object() || !value.contains("uri") ||
            !value["uri"].is_string() || !value.contains("fingerprint") ||
            !value["fingerprint"].is_string() || !value.contains("size") ||
            !value["size"].is_number_unsigned() ||
            !value.contains("archive") || !value["archive"].is_string())
            return std::nullopt;
        IndexedResource resource{value["uri"].get<std::string>(),
                                 value["fingerprint"].get<std::string>(),
                                 value["size"].get<std::uint64_t>(),
                                 value["archive"].get<std::string>()};
        const auto normalized = VFS::NormalizeVirtualPath(resource.uri);
        if (!normalized || *normalized != resource.uri ||
            resource.uri.size() > 4096 || resource.uri <= previous ||
            resource.uri.starts_with(kPackageOverlayPrefix) ||
            !LowerFingerprint(resource.fingerprint) ||
            resource.size > 8ull * 1024ull * 1024ull * 1024ull ||
            !archives.contains(resource.archive) ||
            !entries.emplace(resource.uri, resource).second)
            return std::nullopt;
        previous = resource.uri;
    }
    return entries;
}

std::optional<std::string> FingerprintRegularFile(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status) ||
        psa_crypto_init() != PSA_SUCCESS) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    psa_hash_operation_t digest = PSA_HASH_OPERATION_INIT;
    if (!input || psa_hash_setup(&digest, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return std::nullopt;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && psa_hash_update(&digest, buffer.data(),
                                         static_cast<std::size_t>(count)) !=
                             PSA_SUCCESS) {
            psa_hash_abort(&digest);
            return std::nullopt;
        }
    }
    if (!input.eof()) {
        psa_hash_abort(&digest);
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> hash{};
    std::size_t size = 0;
    if (psa_hash_finish(&digest, hash.data(), hash.size(), &size) !=
            PSA_SUCCESS || size != hash.size()) {
        psa_hash_abort(&digest);
        return std::nullopt;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string fingerprint;
    fingerprint.reserve(64);
    for (const auto byte : hash) {
        fingerprint.push_back(hex[byte >> 4]);
        fingerprint.push_back(hex[byte & 15]);
    }
    return fingerprint;
}

bool IsOverlayMetadataFile(const std::string_view uri) {
    return uri == ".prismatix-output.json" ||
           uri == "Package/manifest.json" ||
           uri == "Package/content-index.json" ||
           uri == "Package/archive-shards.json" ||
           uri == "Package/cli-package-provenance.json" ||
           uri == "Package/entry-cache.json" ||
           (uri.starts_with("Package/entry-cache-") &&
            uri.ends_with(".json"));
}

std::optional<std::map<std::string, std::string>> StaticDistributionFiles(
    const std::filesystem::path& root,
    const sdk::detail::PackageManifest& manifest) {
    std::set<std::string> archives;
    for (const auto& mount : manifest.archives) archives.insert(mount.file);
    std::map<std::string, std::string> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(root, error);
    if (error) return std::nullopt;
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) return std::nullopt;
        const auto status = std::filesystem::symlink_status(
            iterator->path(), error);
        if (error || std::filesystem::is_symlink(status)) return std::nullopt;
        if (std::filesystem::is_directory(status)) continue;
        if (!std::filesystem::is_regular_file(status)) return std::nullopt;
        const auto uri = px::PathToUtf8(
            iterator->path().lexically_relative(root));
        if (archives.contains(uri) || IsOverlayMetadataFile(uri)) continue;
        const auto fingerprint = FingerprintRegularFile(iterator->path());
        if (!fingerprint || !files.emplace(uri, *fingerprint).second)
            return std::nullopt;
    }
    return error ? std::nullopt
                 : std::optional<std::map<std::string, std::string>>{
                       std::move(files)};
}

bool ExistingDirectoryIsWithin(const std::filesystem::path& child,
                               const std::filesystem::path& parent) {
    for (auto current = child; !current.empty();) {
        std::error_code error;
        if (std::filesystem::equivalent(current, parent, error) && !error)
            return true;
        const auto next = current.parent_path();
        if (next == current) break;
        current = next;
    }
    return false;
}

struct OwnedOverlayStage {
    std::filesystem::path path;
    bool created = false;
    ~OwnedOverlayStage() {
        if (!created) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

bool PackagedRuntimeLibrary(const std::filesystem::path& path) {
    std::string name = px::PathToUtf8(path.filename());
    std::ranges::transform(name, name.begin(), [](const unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    if (name.find("shadercross") != std::string::npos ||
        name.find("dxcompiler") != std::string::npos ||
        name == "dxil.dll" || name.find("spirv-cross") != std::string::npos)
        return false;
#ifdef _WIN32
    return name.ends_with(".dll");
#elif defined(__APPLE__)
    return name.ends_with(".dylib");
#else
    return name.ends_with(".so") || name.find(".so.") != std::string::npos;
#endif
}

bool RuntimeInputsMatchBase(
    const std::filesystem::path& base,
    const sdk::detail::PackageManifest& manifest,
    const std::filesystem::path& playerExecutable) {
    const auto baseFiles = StaticDistributionFiles(base, manifest);
    if (!baseFiles) return false;
    const auto playerName = px::PathToUtf8(playerExecutable.filename());
    const auto expectedPlayer = baseFiles->find(playerName);
    if (expectedPlayer == baseFiles->end() ||
        FingerprintRegularFile(playerExecutable) != expectedPlayer->second)
        return false;
    std::set<std::string> seenLibraries;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(
             playerExecutable.parent_path(), error);
         !error && iterator != std::filesystem::directory_iterator();
         iterator.increment(error)) {
        if (!PackagedRuntimeLibrary(iterator->path())) continue;
        const auto name = px::PathToUtf8(iterator->path().filename());
        const auto expected = baseFiles->find(name);
        if (expected == baseFiles->end() ||
            FingerprintRegularFile(iterator->path()) != expected->second)
            return false;
        seenLibraries.insert(name);
    }
    if (error) return false;
    for (const auto& [name, fingerprint] : *baseFiles) {
        (void)fingerprint;
        if (name.find('/') == std::string::npos &&
            PackagedRuntimeLibrary(px::PathFromUtf8(name)) &&
            !seenLibraries.contains(name))
            return false;
    }
    return true;
}

}  // namespace

PackageContentVerification VerifyPackageContents(
    const std::filesystem::path& distribution,
    const PackageVerificationProgress& onProgress,
    const std::stop_token stop) {
    const auto manifestText = ReadBoundedRegularFile(
        distribution / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto indexText = ReadBoundedRegularFile(
        distribution / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    if (!manifestText || !indexText)
        return Fail("Package manifest or content index is missing, unsafe, or oversized");
    const auto parsed = sdk::detail::ParsePackageManifest(*manifestText);
    if (!parsed.Valid()) return Fail("Package manifest is invalid");
    const auto& manifest = parsed.manifest;
    const auto index = Json::parse(*indexText, nullptr, false);
    if (!index.is_object() || !index.contains("format") ||
        index["format"] != "PrismatiXContentIndex" ||
        !index.contains("schemaRevision") || index["schemaRevision"] != 1 ||
        !index.contains("gameId") || index["gameId"] != manifest.gameId ||
        !index.contains("packageFingerprint") ||
        index["packageFingerprint"] != manifest.packageFingerprint ||
        !index.contains("entries") || !index["entries"].is_array() ||
        index["entries"].empty() || index["entries"].size() > 1'000'000)
        return Fail("Content index identity or schema is invalid");

    std::map<std::string, std::vector<IndexedResource>> expected;
    for (const auto& archive : manifest.archives) expected.emplace(archive.file, std::vector<IndexedResource>{});
    std::string previousUri;
    for (const auto& value : index["entries"]) {
        if (!value.is_object() || !value.contains("uri") ||
            !value["uri"].is_string() || !value.contains("fingerprint") ||
            !value["fingerprint"].is_string() || !value.contains("size") ||
            !value["size"].is_number_unsigned() ||
            !value.contains("archive") || !value["archive"].is_string())
            return Fail("Content index has an invalid resource entry");
        const auto uri = value["uri"].get<std::string>();
        const auto fingerprint = value["fingerprint"].get<std::string>();
        const auto size = value["size"].get<std::uint64_t>();
        const auto archive = value["archive"].get<std::string>();
        const auto normalized = VFS::NormalizeVirtualPath(uri);
        const auto mount = expected.find(archive);
        if (!normalized || *normalized != uri || uri.size() > 4096 ||
            uri <= previousUri || !LowerFingerprint(fingerprint) ||
            size > 8ull * 1024ull * 1024ull * 1024ull ||
            mount == expected.end())
            return Fail("Content index has an unsafe, duplicate, or unmounted resource");
        mount->second.push_back({uri, fingerprint, size});
        previousUri = uri;
    }

    const auto key = manifest.encrypted
        ? std::optional<crypto::Key>{crypto::DeriveKey(manifest.archiveKey)}
        : std::nullopt;
    const auto resourceTotal =
        static_cast<std::uint64_t>(index["entries"].size());
    if (onProgress) onProgress(0, resourceTotal, 0);
    std::mutex progressMutex;
    std::uint64_t completedResources = 0;
    std::uint64_t completedBytes = 0;
    std::uint64_t lastReportedResources = 0;
    std::uint64_t lastReportedBytes = 0;
    const auto& expectedByArchive = expected;
    const auto verifyArchive = [&](const std::size_t archiveIndex) -> std::string {
        const auto& archiveMount = manifest.archives[archiveIndex];
        const auto path = distribution / px::PathFromUtf8(archiveMount.file);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
            return "Mounted PDX is missing or not a regular file: " + archiveMount.file;
        Archive archive;
        if (!archive.Open(px::PathToUtf8(path), key ? &*key : nullptr))
            return "Mounted PDX cannot be opened: " + archiveMount.file;
        const auto& resources = expectedByArchive.at(archiveMount.file);
        if (archive.Entries().size() != resources.size())
            return "PDX entry count differs from content index: " + archiveMount.file;
        std::map<std::string, const Archive::Entry*> actualEntries;
        for (const auto& entry : archive.Entries())
            if (!actualEntries.emplace(entry.name, &entry).second)
                return "PDX contains a duplicate resource: " + entry.name;
        for (const auto& resource : resources) {
            if (stop.stop_requested()) return "Package verification was cancelled";
            const auto found = actualEntries.find(resource.uri);
            if (found == actualEntries.end())
                return "Indexed resource is missing from PDX: " + resource.uri;
            auto stream = archive.OpenStreamForVerification(resource.uri);
            if (!stream || stream->Size() != resource.size)
                return "Indexed resource size differs from PDX: " + resource.uri;
            const auto fingerprint = FingerprintStream(*stream, found->second->crc);
            if (!fingerprint || *fingerprint != resource.fingerprint)
                return "Indexed resource SHA-256 or CRC differs from PDX: " + resource.uri;
            {
                std::lock_guard lock(progressMutex);
                ++completedResources;
                completedBytes += resource.size;
            }
        }
        return {};
    };
    std::vector<std::string> archiveErrors(manifest.archives.size());
    std::vector<std::uint8_t> archiveCompleted(manifest.archives.size(), 0);
    std::atomic<std::size_t> nextArchive{0};
    std::atomic<bool> failed{false};
    std::atomic<unsigned> completedWorkers{0};
    const auto work = [&] {
        while (!failed.load(std::memory_order_relaxed) &&
               !stop.stop_requested()) {
            const auto archiveIndex = nextArchive.fetch_add(1, std::memory_order_relaxed);
            if (archiveIndex >= manifest.archives.size()) break;
            try {
                archiveErrors[archiveIndex] = verifyArchive(archiveIndex);
            } catch (const std::exception& error) {
                archiveErrors[archiveIndex] =
                    "Package archive verification failed unexpectedly: " +
                    std::string(error.what());
            } catch (...) {
                archiveErrors[archiveIndex] =
                    "Package archive verification failed unexpectedly";
            }
            if (!archiveErrors[archiveIndex].empty())
                failed.store(true, std::memory_order_relaxed);
            else archiveCompleted[archiveIndex] = 1;
        }
        completedWorkers.fetch_add(1, std::memory_order_relaxed);
    };
    {
        std::vector<std::jthread> threads;
        const auto workers = VerificationWorkers(manifest.archives.size(),
                                                 manifest.encrypted);
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker)
            threads.emplace_back(work);
        if (onProgress) {
            while (completedWorkers.load(std::memory_order_relaxed) < workers) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::uint64_t current = 0;
                std::uint64_t rawBytes = 0;
                {
                    std::lock_guard lock(progressMutex);
                    current = completedResources;
                    rawBytes = completedBytes;
                }
                if (current == resourceTotal ||
                    current - lastReportedResources >= 256 ||
                    rawBytes - lastReportedBytes >=
                        64ull * 1024ull * 1024ull) {
                    onProgress(current, resourceTotal, rawBytes);
                    lastReportedResources = current;
                    lastReportedBytes = rawBytes;
                }
            }
        }
    }
    for (const auto& error : archiveErrors)
        if (!error.empty()) return Fail(error);
    if (stop.stop_requested()) return Fail("Package verification was cancelled");
    PackageContentVerification result;
    result.archives = static_cast<std::uint64_t>(
        std::ranges::count(archiveCompleted, std::uint8_t{1}));
    result.resources = completedResources;
    result.rawBytes = completedBytes;
    if (result.archives != manifest.archives.size() ||
        result.resources != resourceTotal)
        return Fail("Package archive verification ended without covering every resource");
    if (onProgress && lastReportedResources != result.resources)
        onProgress(result.resources, resourceTotal, result.rawBytes);
    result.valid = true;
    return result;
}

static PreverifiedOverlayBase VerifyBaseForOverlay(
    const std::filesystem::path& distribution,
    const PackageOverlayBuildProgress& onProgress,
    const std::stop_token stop = {}) {
    PreverifiedOverlayBase result;
    std::error_code error;
    result.directory = std::filesystem::canonical(distribution, error);
    if (error || !std::filesystem::is_directory(result.directory)) {
        result.error = "Base distribution is not a readable directory";
        return result;
    }
    const auto manifest = ReadBoundedRegularFile(
        result.directory / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto index = ReadBoundedRegularFile(
        result.directory / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    if (!manifest || !index) {
        result.error = "Base package documents are unavailable or oversized";
        return result;
    }
#ifdef _WIN32
    auto locks = LockBaseArchives(result.directory, *manifest);
    if (!locks) {
        result.error = "Base package archives cannot be held read-only for verification";
        return result;
    }
    result.readLocks = std::move(*locks);
#endif
    const auto before = BaseArchiveStamps(result.directory, *manifest);
    if (!before) {
        result.error = "Base package archives are missing or unsafe";
        return result;
    }
    result.manifest = *manifest;
    result.index = *index;
    result.contents = VerifyPackageContents(
        result.directory, [&](const std::uint64_t current,
                              const std::uint64_t total,
                              const std::uint64_t rawBytes) {
            if (onProgress) onProgress("verify-base", current, total, rawBytes);
        }, stop);
    if (!result.contents.valid) {
        result.error = "Base distribution failed content verification: " +
                       result.contents.error;
        return result;
    }
    const auto after = BaseArchiveStamps(result.directory, result.manifest);
    if (!after || *after != *before ||
        ReadBoundedRegularFile(result.directory / "Package/manifest.json",
                               4ull * 1024ull * 1024ull) != manifest ||
        ReadBoundedRegularFile(result.directory / "Package/content-index.json",
                               128ull * 1024ull * 1024ull) != index) {
        result.error = "Base package changed during content verification";
        return result;
    }
    result.archives = std::move(*after);
    return result;
}

static PackageOverlayVerification VerifyOverlayImpl(
    const std::filesystem::path& distribution,
    const std::filesystem::path& overlayPath,
    const bool requireSignature,
    const std::optional<std::pair<std::string_view, std::string_view>>
        baseDocuments = std::nullopt) {
    const auto fail = [](std::string message) {
        return PackageOverlayVerification{.error = std::move(message)};
    };
    std::optional<std::string> baseManifestFile;
    std::optional<std::string> baseIndexFile;
    if (!baseDocuments) {
        baseManifestFile = ReadBoundedRegularFile(
            distribution / "Package/manifest.json", 4ull * 1024ull * 1024ull);
        baseIndexFile = ReadBoundedRegularFile(
            distribution / "Package/content-index.json",
            128ull * 1024ull * 1024ull);
    }
    const std::optional<std::string_view> baseManifestText = baseDocuments
        ? std::optional<std::string_view>{baseDocuments->first}
        : (baseManifestFile
               ? std::optional<std::string_view>{*baseManifestFile}
               : std::nullopt);
    const std::optional<std::string_view> baseIndexText = baseDocuments
        ? std::optional<std::string_view>{baseDocuments->second}
        : (baseIndexFile
               ? std::optional<std::string_view>{*baseIndexFile}
               : std::nullopt);
    if (!baseManifestText || !baseIndexText)
        return fail("Base package manifest or content index is unavailable");
    if (baseManifestText->empty() || baseManifestText->size() > 4ull * 1024ull * 1024ull ||
        baseIndexText->empty() || baseIndexText->size() > 128ull * 1024ull * 1024ull)
        return fail("Base package manifest or content index is oversized");
    const auto baseParsed =
        sdk::detail::ParsePackageManifest(*baseManifestText);
    if (!baseParsed.Valid()) return fail("Base package manifest is invalid");
    const auto baseEntries =
        ParseIndexedResources(*baseIndexText, baseParsed.manifest);
    if (!baseEntries) return fail("Base content index is invalid");

    std::error_code error;
    const auto status = std::filesystem::symlink_status(overlayPath, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        return fail("Overlay PDX is missing or is not a regular file");
    Archive overlay;
    if (!overlay.Open(px::PathToUtf8(overlayPath)))
        return fail("Overlay PDX is unreadable or encrypted");
    const auto metadataText = ReadBoundedArchiveText(
        overlay, kPackageOverlayManifestUri, 16ull * 1024ull);
    const auto targetManifestText = ReadBoundedArchiveText(
        overlay, kPackageOverlayTargetManifestUri, 4ull * 1024ull * 1024ull);
    const auto targetIndexText = ReadBoundedArchiveText(
        overlay, kPackageOverlayTargetIndexUri, 128ull * 1024ull * 1024ull);
    if (!metadataText || !targetManifestText || !targetIndexText)
        return fail("Overlay metadata is missing, damaged, or oversized");

    const Json metadata = Json::parse(*metadataText, nullptr, false);
    static const std::set<std::string> baseFields{
        "format", "schemaRevision", "gameId", "basePackageFingerprint",
        "targetPackageFingerprint", "baseManifestSha256",
        "baseContentIndexSha256", "targetManifestSha256",
        "targetContentIndexSha256"};
    if (!metadata.is_object() ||
        !metadata.contains("schemaRevision") ||
        !metadata["schemaRevision"].is_number_integer() ||
        (metadata["schemaRevision"] != 1 &&
         metadata["schemaRevision"] != 2 &&
         metadata["schemaRevision"] != 3))
        return fail("Overlay metadata schema is invalid");
    const int revision = metadata["schemaRevision"].get<int>();
    const bool optionalOverlay = revision == 3;
    if (metadata.size() != baseFields.size() + (optionalOverlay ? 2 : 0))
        return fail("Overlay metadata schema is invalid");
    if (optionalOverlay &&
        (!metadata.contains("kind") || metadata["kind"] != "optional" ||
         !metadata.contains("optionalId") ||
         !metadata["optionalId"].is_string() ||
         !ValidOptionalPackId(metadata["optionalId"].get<std::string>())))
        return fail("Optional overlay identity is invalid");
    const bool signedOverlay = revision >= 2;
    if (requireSignature && !signedOverlay)
        return fail("Automatic overlay loading requires a publisher signature");
    for (const auto& field : baseFields) {
        if (!metadata.contains(field) ||
            (field != "schemaRevision" && !metadata[field].is_string()))
            return fail("Overlay metadata contains a missing or invalid field");
    }
    if (metadata["format"] != "PrismatiXPackageOverlay" ||
        metadata["gameId"] != baseParsed.manifest.gameId ||
        metadata["basePackageFingerprint"] !=
            baseParsed.manifest.packageFingerprint)
        return fail("Overlay targets a different game or base package");
    if (signedOverlay) {
        if (baseParsed.manifest.publisherPublicKey.empty())
            return fail("Base package has no pinned publisher public key");
        const auto signature = ReadBoundedArchiveText(
            overlay, kPackageOverlaySignatureUri, 64);
        if (!signature || signature->size() != 64)
            return fail("Overlay publisher signature is missing or damaged");
        constexpr char digits[] = "0123456789abcdef";
        std::string signatureHex;
        signatureHex.reserve(128);
        for (const unsigned char byte : *signature) {
            signatureHex.push_back(digits[byte >> 4]);
            signatureHex.push_back(digits[byte & 15]);
        }
        if (!VerifyOverlayMetadataSignature(
                *metadataText, signatureHex,
                baseParsed.manifest.publisherPublicKey, revision))
            return fail("Overlay publisher signature is invalid");
    }
    const auto baseManifestSha = FingerprintText(*baseManifestText);
    const auto baseIndexSha = FingerprintText(*baseIndexText);
    const auto targetManifestSha = FingerprintText(*targetManifestText);
    const auto targetIndexSha = FingerprintText(*targetIndexText);
    if (!baseManifestSha || !baseIndexSha || !targetManifestSha ||
        !targetIndexSha ||
        metadata["baseManifestSha256"] != *baseManifestSha ||
        metadata["baseContentIndexSha256"] != *baseIndexSha ||
        metadata["targetManifestSha256"] != *targetManifestSha ||
        metadata["targetContentIndexSha256"] != *targetIndexSha)
        return fail("Overlay metadata does not match its exact base and target documents");

    const auto targetParsed =
        sdk::detail::ParsePackageManifest(*targetManifestText);
    if (!targetParsed.Valid() ||
        targetParsed.manifest.gameId != baseParsed.manifest.gameId ||
        targetParsed.manifest.engineVersion != baseParsed.manifest.engineVersion ||
        targetParsed.manifest.publisherPublicKey !=
            baseParsed.manifest.publisherPublicKey ||
        targetParsed.manifest.packageFingerprint ==
            baseParsed.manifest.packageFingerprint ||
        metadata["targetPackageFingerprint"] !=
            targetParsed.manifest.packageFingerprint)
        return fail("Overlay target manifest is incompatible or invalid");
    if (optionalOverlay &&
        (targetParsed.manifest.contentVersion !=
             baseParsed.manifest.contentVersion ||
         targetParsed.manifest.saveVersion != baseParsed.manifest.saveVersion ||
         targetParsed.manifest.persistenceDataId !=
             baseParsed.manifest.persistenceDataId ||
         targetParsed.manifest.persistenceKeyId !=
             baseParsed.manifest.persistenceKeyId))
        return fail("Optional overlay cannot change save or content version identity");
    const auto targetEntries =
        ParseIndexedResources(*targetIndexText, targetParsed.manifest);
    if (!targetEntries) return fail("Overlay target content index is invalid");
    const auto indexed = [&targetEntries](const std::string& uri) {
        return targetEntries->contains(uri);
    };
    if (!indexed(targetParsed.manifest.startRuntimeIr) ||
        !indexed(targetParsed.manifest.sourceMap))
        return fail("Overlay target index omits its Runtime IR or source map");
    for (const auto& route : targetParsed.manifest.routes)
        if (!indexed(route.scene))
            return fail("Overlay target index omits a route scene: " + route.scene);
    for (const auto& migration : targetParsed.manifest.saveMigrations)
        if (!indexed(migration.asset))
            return fail("Overlay target index omits a save migration: " + migration.asset);
    for (const auto& effect : targetParsed.manifest.customEffects)
        for (const auto& artifact : effect.artifacts)
            if (!indexed(artifact.asset))
                return fail("Overlay target index omits a shader artifact: " + artifact.asset);

    std::set<std::string> expectedNames{
        std::string(kPackageOverlayManifestUri),
        std::string(kPackageOverlayTargetManifestUri),
        std::string(kPackageOverlayTargetIndexUri)};
    if (signedOverlay)
        expectedNames.insert(std::string(kPackageOverlaySignatureUri));
    PackageOverlayVerification result;
    for (const auto& [uri, target] : *targetEntries) {
        const auto base = baseEntries->find(uri);
        if (base != baseEntries->end() &&
            base->second.fingerprint == target.fingerprint &&
            base->second.size == target.size) continue;
        expectedNames.insert(uri);
        auto stream = overlay.OpenStream(uri);
        if (!stream || stream->Size() != target.size)
            return fail("Overlay changed resource is missing or has the wrong size: " + uri);
        const auto fingerprint = FingerprintStream(*stream);
        if (!fingerprint || *fingerprint != target.fingerprint)
            return fail("Overlay changed resource differs from target index: " + uri);
        ++result.changedResources;
        result.changedRawBytes += target.size;
    }
    for (const auto& [uri, base] : *baseEntries) {
        if (!targetEntries->contains(uri)) result.tombstones.push_back(uri);
    }
    if (overlay.Entries().size() != expectedNames.size())
        return fail("Overlay PDX has missing or unexpected entries");
    for (const auto& entry : overlay.Entries()) {
        if (!expectedNames.contains(entry.name))
            return fail("Overlay PDX has an unexpected entry: " + entry.name);
    }
    result.targetManifestJson = *targetManifestText;
    result.targetContentIndexJson = *targetIndexText;
    if (optionalOverlay)
        result.optionalPackId = metadata["optionalId"].get<std::string>();
    result.valid = true;
    return result;
}

PackageOverlayVerification VerifyPackageOverlay(
    const std::filesystem::path& distribution,
    const std::filesystem::path& overlayPath) {
    return VerifyOverlayImpl(distribution, overlayPath, false);
}

PackageOverlayVerification VerifySignedPackageOverlay(
    const std::filesystem::path& distribution,
    const std::filesystem::path& overlayPath) {
    return VerifyOverlayImpl(distribution, overlayPath, true);
}

PackageOverlayVerification VerifySignedPackageOverlayFromDocuments(
    const std::string_view baseManifestJson,
    const std::string_view baseContentIndexJson,
    const std::filesystem::path& overlayPath) {
    return VerifyOverlayImpl({}, overlayPath, true,
                             std::pair{baseManifestJson,
                                       baseContentIndexJson});
}

PackageOverlayChain ResolveSignedPackageOverlayChain(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& updatesDirectory,
    const std::string_view enabledOptionalPackId,
    const std::filesystem::path& excludedOverlayPath) {
    const auto fail = [](std::string message) {
        return PackageOverlayChain{.error = std::move(message)};
    };
    if (!enabledOptionalPackId.empty() &&
        !ValidOptionalPackId(enabledOptionalPackId))
        return fail("Enabled optional pack ID is invalid");
    std::error_code error;
    const auto directoryStatus = std::filesystem::symlink_status(
        updatesDirectory, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && directoryStatus.type() ==
                       std::filesystem::file_type::not_found))
        return {.valid = true};
    if (error || !std::filesystem::is_directory(directoryStatus) ||
        std::filesystem::is_symlink(directoryStatus))
        return fail("Updates path is not a regular directory");

    std::vector<std::filesystem::path> candidates;
    std::filesystem::directory_iterator iterator(updatesDirectory, error);
    if (error) return fail("Updates directory cannot be listed");
    const std::filesystem::directory_iterator end;
    std::size_t inspected = 0;
    for (; iterator != end; iterator.increment(error)) {
        if (error || ++inspected > 256)
            return fail("Updates directory has too many or unreadable entries");
        if (iterator->path().extension() != ".pdx") continue;
        if (!excludedOverlayPath.empty()) {
            std::error_code equivalentError;
            if ((std::filesystem::equivalent(iterator->path(),
                                             excludedOverlayPath,
                                             equivalentError) &&
                 !equivalentError) ||
                iterator->path().lexically_normal() ==
                    excludedOverlayPath.lexically_normal())
                continue;
        }
        const auto status = std::filesystem::symlink_status(iterator->path(), error);
        if (error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
            return fail("Update PDX is not a regular file");
        const auto size = std::filesystem::file_size(iterator->path(), error);
        if (error || size == 0 || size > 64ull * 1024ull * 1024ull * 1024ull ||
            candidates.size() >= 64)
            return fail("Update PDX is oversized or there are too many updates");
        candidates.push_back(iterator->path());
    }
    if (error) return fail("Updates directory cannot be listed");
    std::ranges::sort(candidates);
    if (candidates.empty()) return {.valid = true};
    // These are untrusted routing hints. An exact-base candidate still needs
    // the full signature, document, and changed-resource verification below.
    // Skipping a different base avoids re-reading every stale PDX at each
    // depth of a large update chain.
    struct CandidateHint {
        std::string baseFingerprint;
        std::optional<std::string> optionalId;
    };
    std::vector<CandidateHint> hints;
    hints.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        Archive archive;
        if (!archive.Open(px::PathToUtf8(candidate))) {
            hints.emplace_back();
            continue;
        }
        const auto text = ReadBoundedArchiveText(
            archive, kPackageOverlayManifestUri, 16ull * 1024ull);
        if (!text) {
            hints.emplace_back();
            continue;
        }
        const Json metadata = Json::parse(*text, nullptr, false);
        CandidateHint hint;
        if (metadata.is_object() &&
            metadata.contains("basePackageFingerprint") &&
            metadata["basePackageFingerprint"].is_string())
            hint.baseFingerprint =
                metadata["basePackageFingerprint"].get<std::string>();
        if (metadata.is_object() &&
            metadata.contains("schemaRevision") &&
            metadata["schemaRevision"].is_number_integer() &&
            metadata["schemaRevision"] == 3 &&
            metadata.contains("kind") &&
            metadata["kind"].is_string() &&
            metadata["kind"] == "optional" &&
            metadata.contains("optionalId") &&
            metadata["optionalId"].is_string() &&
            ValidOptionalPackId(metadata["optionalId"].get<std::string>()))
            hint.optionalId = metadata["optionalId"].get<std::string>();
        hints.push_back(std::move(hint));
    }

    auto currentManifest = ReadBoundedRegularFile(
        baseDistribution / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    auto currentIndex = ReadBoundedRegularFile(
        baseDistribution / "Package/content-index.json",
        128ull * 1024ull * 1024ull);
    if (!currentManifest || !currentIndex)
        return fail("Base package manifest or content index is unavailable");
    const auto base = sdk::detail::ParsePackageManifest(*currentManifest);
    if (!base.Valid()) return fail("Base package manifest is invalid");
    std::set<std::string> visited{base.manifest.packageFingerprint};
    struct SaveIdentity {
        std::string fingerprint;
        std::string contentVersion;
        std::uint32_t saveVersion = 0;
    };
    std::vector<SaveIdentity> ancestors{{base.manifest.packageFingerprint,
                                         base.manifest.contentVersion,
                                         base.manifest.saveVersion}};
    std::vector<bool> consumed(candidates.size());
    PackageOverlayChain result;
    for (std::size_t depth = 0; depth < candidates.size(); ++depth) {
        std::optional<std::pair<std::size_t, PackageOverlayVerification>> next;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (consumed[index] || hints[index].optionalId ||
                hints[index].baseFingerprint !=
                    ancestors.back().fingerprint) continue;
            auto verified = VerifySignedPackageOverlayFromDocuments(
                *currentManifest, *currentIndex, candidates[index]);
            if (!verified.valid) continue;
            if (!verified.optionalPackId.empty()) continue;
            if (next)
                return fail("More than one signed update applies to the same package version");
            next.emplace(index, std::move(verified));
        }
        if (!next) break;
        const auto target = sdk::detail::ParsePackageManifest(
            next->second.targetManifestJson);
        if (!target.Valid() ||
            !visited.insert(target.manifest.packageFingerprint).second)
            return fail("Signed update chain repeats a package version");
        ancestors.push_back({target.manifest.packageFingerprint,
                             target.manifest.contentVersion,
                             target.manifest.saveVersion});
        consumed[next->first] = true;
        result.mounts.push_back({candidates[next->first],
                                 std::move(next->second.tombstones)});
        *currentManifest = std::move(next->second.targetManifestJson);
        *currentIndex = std::move(next->second.targetContentIndexJson);
    }
    if (!result.mounts.empty()) {
        result.targetManifestJson = *currentManifest;
        result.targetContentIndexJson = *currentIndex;
    }
    const auto main = sdk::detail::ParsePackageManifest(*currentManifest);
    if (!main.Valid()) return fail("Signed update chain target is invalid");
    result.mainPackageFingerprint = main.manifest.packageFingerprint;
    result.mainCompatibleSaveFingerprints =
        main.manifest.compatibleSaveFingerprints;
    for (const auto& reader : main.manifest.legacyPersistenceReaders)
        result.mainLegacyPersistenceSecrets.push_back(reader.secret);
    for (const auto& ancestor : ancestors) {
        if (ancestor.fingerprint != result.mainPackageFingerprint &&
            ancestor.contentVersion == main.manifest.contentVersion &&
            ancestor.saveVersion == main.manifest.saveVersion)
            result.compatibleSaveFingerprints.push_back(ancestor.fingerprint);
    }
    if (!enabledOptionalPackId.empty()) {
        std::optional<std::pair<std::size_t, PackageOverlayVerification>> selected;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (consumed[index] || !hints[index].optionalId ||
                *hints[index].optionalId != enabledOptionalPackId ||
                hints[index].baseFingerprint !=
                    result.mainPackageFingerprint) continue;
            auto verified = VerifySignedPackageOverlayFromDocuments(
                *currentManifest, *currentIndex, candidates[index]);
            if (!verified.valid ||
                verified.optionalPackId != enabledOptionalPackId) continue;
            if (selected)
                return fail("More than one signed optional pack matches the enabled ID");
            selected.emplace(index, std::move(verified));
        }
        if (selected) {
            consumed[selected->first] = true;
            result.mounts.push_back({candidates[selected->first],
                                     std::move(selected->second.tombstones)});
            result.targetManifestJson =
                std::move(selected->second.targetManifestJson);
            result.targetContentIndexJson =
                std::move(selected->second.targetContentIndexJson);
            result.optionalPackId = std::string(enabledOptionalPackId);
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index)
        if (!consumed[index] &&
            (!hints[index].optionalId ||
             *hints[index].optionalId == enabledOptionalPackId))
            result.ignoredFiles.push_back(px::PathToUtf8(
                candidates[index].filename()));
    result.valid = true;
    return result;
}

PackageOverlayBuildResult CreatePackageOverlay(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& targetDistribution,
    const std::filesystem::path& outputPath,
    const PackageOverlayBuildProgress& onProgress,
    const std::filesystem::path& signingKeyFile,
    const std::string_view optionalPackId) {
    const auto fail = [](std::string message) {
        return PackageOverlayBuildResult{.error = std::move(message)};
    };
    if (!baseDistribution.is_absolute() ||
        !targetDistribution.is_absolute() || !outputPath.is_absolute() ||
        outputPath.extension() != ".pdx" || outputPath.filename().empty())
        return fail("Base, target, and a new .pdx output must be absolute paths");
    std::error_code error;
    const auto base = std::filesystem::canonical(baseDistribution, error);
    if (error || !std::filesystem::is_directory(base))
        return fail("Base distribution is not a readable directory");
    error.clear();
    const auto target = std::filesystem::canonical(targetDistribution, error);
    if (error || !std::filesystem::is_directory(target) || target == base)
        return fail("Target distribution is missing or is the base directory");
    error.clear();
    const auto outputParent =
        std::filesystem::canonical(outputPath.parent_path(), error);
    if (error || !std::filesystem::is_directory(outputParent) ||
        ExistingDirectoryIsWithin(outputParent, base) ||
        ExistingDirectoryIsWithin(outputParent, target))
        return fail("Overlay output parent is missing or inside a distribution");
    error.clear();
    if (std::filesystem::exists(outputPath, error) || error)
        return fail("Overlay output already exists or cannot be inspected");

    const auto baseManifestBefore = ReadBoundedRegularFile(
        base / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto baseIndexBefore = ReadBoundedRegularFile(
        base / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    const auto targetManifestBefore = ReadBoundedRegularFile(
        target / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto targetIndexBefore = ReadBoundedRegularFile(
        target / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    if (!baseManifestBefore || !baseIndexBefore || !targetManifestBefore ||
        !targetIndexBefore)
        return fail("A distribution manifest or content index is unavailable");

    const auto baseVerified = VerifyPackageContents(
        base, [&](const std::uint64_t current, const std::uint64_t total,
                  const std::uint64_t rawBytes) {
            if (onProgress)
                onProgress("verify-base", current, total, rawBytes);
        });
    if (!baseVerified.valid)
        return fail("Base distribution failed content verification: " +
                    baseVerified.error);
    const auto targetVerified = VerifyPackageContents(
        target, [&](const std::uint64_t current, const std::uint64_t total,
                    const std::uint64_t rawBytes) {
            if (onProgress)
                onProgress("verify-target", current, total, rawBytes);
        });
    if (!targetVerified.valid)
        return fail("Target distribution failed content verification: " +
                    targetVerified.error);

    const auto baseManifestText = ReadBoundedRegularFile(
        base / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto baseIndexText = ReadBoundedRegularFile(
        base / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    const auto targetManifestText = ReadBoundedRegularFile(
        target / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto targetIndexText = ReadBoundedRegularFile(
        target / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    if (baseManifestText != baseManifestBefore ||
        baseIndexText != baseIndexBefore ||
        targetManifestText != targetManifestBefore ||
        targetIndexText != targetIndexBefore)
        return fail("A distribution manifest or content index changed after verification");
    const auto baseParsed =
        sdk::detail::ParsePackageManifest(*baseManifestText);
    const auto targetParsed =
        sdk::detail::ParsePackageManifest(*targetManifestText);
    if (!baseParsed.Valid() || !targetParsed.Valid() ||
        baseParsed.manifest.gameId != targetParsed.manifest.gameId ||
        baseParsed.manifest.engineVersion != targetParsed.manifest.engineVersion ||
        baseParsed.manifest.publisherPublicKey !=
            targetParsed.manifest.publisherPublicKey ||
        baseParsed.manifest.packageFingerprint ==
            targetParsed.manifest.packageFingerprint)
        return fail("Base and target are not distinct compatible package versions");
    const bool signedOverlay = !signingKeyFile.empty();
    if (!optionalPackId.empty() &&
        (!ValidOptionalPackId(optionalPackId) || !signedOverlay))
        return fail("Optional overlays require a valid ID and publisher signature");
    if (!optionalPackId.empty() &&
        (baseParsed.manifest.contentVersion !=
             targetParsed.manifest.contentVersion ||
         baseParsed.manifest.saveVersion != targetParsed.manifest.saveVersion ||
         baseParsed.manifest.persistenceDataId !=
             targetParsed.manifest.persistenceDataId ||
         baseParsed.manifest.persistenceKeyId !=
             targetParsed.manifest.persistenceKeyId))
        return fail("Optional overlays cannot change save or content version identity");
    if (signedOverlay && baseParsed.manifest.publisherPublicKey.empty())
        return fail("Signed overlay requires a pinned publisher public key");
    const auto baseEntries =
        ParseIndexedResources(*baseIndexText, baseParsed.manifest);
    const auto targetEntries =
        ParseIndexedResources(*targetIndexText, targetParsed.manifest);
    if (!baseEntries || !targetEntries)
        return fail("A distribution content index changed after verification");
    const auto baseStatic = StaticDistributionFiles(base, baseParsed.manifest);
    const auto targetStatic =
        StaticDistributionFiles(target, targetParsed.manifest);
    if (!baseStatic || !targetStatic || *baseStatic != *targetStatic)
        return fail("Player/runtime files differ; a content-only overlay cannot update them");

    std::vector<IndexedResource> changed;
    for (const auto& [uri, resource] : *targetEntries) {
        const auto old = baseEntries->find(uri);
        if (old == baseEntries->end() ||
            old->second.fingerprint != resource.fingerprint ||
            old->second.size != resource.size)
            changed.push_back(resource);
    }
    std::uint64_t removed = 0;
    for (const auto& [uri, resource] : *baseEntries)
        if (!targetEntries->contains(uri)) ++removed;
    const auto baseManifestHash = FingerprintText(*baseManifestText);
    const auto baseIndexHash = FingerprintText(*baseIndexText);
    const auto targetManifestHash = FingerprintText(*targetManifestText);
    const auto targetIndexHash = FingerprintText(*targetIndexText);
    if (!baseManifestHash || !baseIndexHash || !targetManifestHash ||
        !targetIndexHash)
        return fail("Overlay metadata hashing failed");
    Json metadata{
        {"format", "PrismatiXPackageOverlay"},
        {"schemaRevision", optionalPackId.empty() ? (signedOverlay ? 2 : 1) : 3},
        {"gameId", baseParsed.manifest.gameId},
        {"basePackageFingerprint", baseParsed.manifest.packageFingerprint},
        {"targetPackageFingerprint", targetParsed.manifest.packageFingerprint},
        {"baseManifestSha256", *baseManifestHash},
        {"baseContentIndexSha256", *baseIndexHash},
        {"targetManifestSha256", *targetManifestHash},
        {"targetContentIndexSha256", *targetIndexHash}};
    if (!optionalPackId.empty()) {
        metadata["kind"] = "optional";
        metadata["optionalId"] = optionalPackId;
    }
    const std::string metadataText = metadata.dump();
    if (metadataText.size() > 16ull * 1024ull)
        return fail("Overlay metadata exceeds its format limit");
    std::optional<Bytes> signature;
    if (signedOverlay) {
        auto keyText = ReadBoundedRegularFile(signingKeyFile, 256);
        if (!keyText)
            return fail("Overlay signing key file is missing or unsafe");
        const auto first = keyText->find_first_not_of(" \r\n\t");
        const auto last = keyText->find_last_not_of(" \r\n\t");
        if (first == std::string::npos)
            return fail("Overlay signing key file is empty");
        auto keyHex = keyText->substr(first, last - first + 1);
        const auto signedHex = SignOverlayMetadata(
            metadataText, keyHex, baseParsed.manifest.publisherPublicKey,
            optionalPackId.empty() ? 2 : 3);
        std::fill(keyHex.begin(), keyHex.end(), '\0');
        std::fill(keyText->begin(), keyText->end(), '\0');
        if (!signedHex)
            return fail("Overlay signing key does not match the pinned publisher key");
        signature.emplace();
        signature->reserve(64);
        for (std::size_t index = 0; index < signedHex->size(); index += 2) {
            const auto nibble = [](const char value) -> std::uint8_t {
                return value <= '9' ? value - '0' : value - 'a' + 10;
            };
            signature->push_back(static_cast<std::uint8_t>(
                (nibble((*signedHex)[index]) << 4) |
                nibble((*signedHex)[index + 1])));
        }
    }

    const auto stagedName =
        px::PathToUtf8(outputPath.filename()) + ".staging-" +
        baseManifestHash->substr(0, 8) + targetManifestHash->substr(0, 8);
    OwnedOverlayStage stage{outputParent / px::PathFromUtf8(stagedName)};
    error.clear();
    if (!std::filesystem::create_directory(stage.path, error) || error)
        return fail("A prior overlay staging directory exists or cannot be created");
    stage.created = true;

    ArchiveStreamWriter writer;
    const auto addText = [&writer](const std::string_view uri,
                                   const std::string& value) {
        writer.AddData(std::string(uri), Bytes(value.begin(), value.end()));
    };
    addText(kPackageOverlayManifestUri, metadataText);
    addText(kPackageOverlayTargetManifestUri, *targetManifestText);
    addText(kPackageOverlayTargetIndexUri, *targetIndexText);
    if (signature)
        writer.AddData(std::string(kPackageOverlaySignatureUri),
                       std::move(*signature));
    const auto targetKey = targetParsed.manifest.encrypted
        ? std::optional<crypto::Key>{
              crypto::DeriveKey(targetParsed.manifest.archiveKey)}
        : std::nullopt;
    std::unordered_map<std::string, std::unique_ptr<Archive>> archives;
    std::uint64_t plannedBytes = 0;
    if (onProgress) onProgress("plan-changed", 0, changed.size(), 0);
    for (std::size_t index = 0; index < changed.size(); ++index) {
        const auto& resource = changed[index];
        auto found = archives.find(resource.archive);
        if (found == archives.end()) {
            const auto path = target / px::PathFromUtf8(resource.archive);
            error.clear();
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_regular_file(status) ||
                std::filesystem::is_symlink(status))
                return fail("Target PDX disappeared: " + resource.archive);
            auto archive = std::make_unique<Archive>();
            if (!archive->Open(px::PathToUtf8(path),
                               targetKey ? &*targetKey : nullptr))
                return fail("Target PDX changed: " + resource.archive);
            found = archives.emplace(resource.archive, std::move(archive)).first;
        }
        Archive* source = found->second.get();
        writer.AddStream(
            resource.uri,
            [source, uri = resource.uri] { return source->OpenStream(uri); },
            resource.size, resource.fingerprint,
            resource.size >= 64ull * 1024ull);
        if (resource.size >
            (std::numeric_limits<std::uint64_t>::max)() - plannedBytes)
            return fail("Changed resource total exceeds the overlay limit");
        plannedBytes += resource.size;
        if (onProgress)
            onProgress("plan-changed", index + 1, changed.size(),
                       plannedBytes);
    }
    const auto stagedPdx = stage.path / "patch.pdx";
    if (!writer.Write(px::PathToUtf8(stagedPdx),
                      [&](const std::uint64_t current,
                          const std::uint64_t total) {
                          if (onProgress)
                              onProgress("write-overlay", current, total,
                                         current);
                      }))
        return fail("Overlay PDX could not be written from changed resources");
    const auto verified = signedOverlay
        ? VerifySignedPackageOverlay(base, stagedPdx)
        : VerifyPackageOverlay(base, stagedPdx);
    if (!verified.valid || verified.changedResources != changed.size() ||
        verified.tombstones.size() != removed)
        return fail("Created overlay failed verification: " + verified.error);
    if (StaticDistributionFiles(base, baseParsed.manifest) != baseStatic ||
        StaticDistributionFiles(target, targetParsed.manifest) != targetStatic)
        return fail("Distribution runtime files changed during overlay creation");
    if (ReadBoundedRegularFile(base / "Package/manifest.json",
                               4ull * 1024ull * 1024ull) != baseManifestBefore ||
        ReadBoundedRegularFile(base / "Package/content-index.json",
                               128ull * 1024ull * 1024ull) != baseIndexBefore ||
        ReadBoundedRegularFile(target / "Package/manifest.json",
                               4ull * 1024ull * 1024ull) != targetManifestBefore ||
        ReadBoundedRegularFile(target / "Package/content-index.json",
                               128ull * 1024ull * 1024ull) != targetIndexBefore)
        return fail("Distribution metadata changed during overlay creation");
#ifdef _WIN32
    // The stage and output share a parent, so this move is on one volume.
    // Omitting MOVEFILE_REPLACE_EXISTING makes a late competing output fail
    // atomically, including on volumes that do not support hard links.
    if (!MoveFileExW(stagedPdx.c_str(), outputPath.c_str(),
                     MOVEFILE_WRITE_THROUGH))
        return fail("Overlay could not be published without replacing existing data: "
                    "Win32 error " + std::to_string(GetLastError()));
#else
    error.clear();
    std::filesystem::create_hard_link(stagedPdx, outputPath, error);
    if (error)
        return fail("Overlay could not be published without replacing existing data: " +
                    error.message());
#endif
    return {.created = true,
            .changedResources = verified.changedResources,
            .removedResources = removed,
            .changedRawBytes = verified.changedRawBytes};
}

static PackageOverlayBuildResult CreatePackageOverlayFromPlanImpl(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& artifactRoot,
    const sdk::PackagePlanResult& plan,
    const std::filesystem::path& playerExecutable,
    const std::filesystem::path& outputPath,
    const PackageOverlayBuildProgress& onProgress,
    const std::filesystem::path& signingKeyFile,
    const std::string_view optionalPackId,
    const std::filesystem::path& priorUpdatesDirectory,
    const bool planOnly,
    const PreverifiedOverlayBase* preverifiedBase) {
    const auto fail = [](std::string message) {
        return PackageOverlayBuildResult{.error = std::move(message)};
    };
    if (!plan.valid || !plan.allInputsVerified)
        return fail("Direct overlay requires a complete AllInputs package plan");
    if (!baseDistribution.is_absolute() || !artifactRoot.is_absolute() ||
        !playerExecutable.is_absolute() || !outputPath.is_absolute() ||
        outputPath.extension() != ".pdx" || outputPath.filename().empty())
        return fail("Base, artifact, Player, and new .pdx output must be absolute paths");
    std::error_code error;
    const auto base = std::filesystem::canonical(baseDistribution, error);
    if (error || !std::filesystem::is_directory(base))
        return fail("Base distribution is not a readable directory");
    error.clear();
    const auto artifact = std::filesystem::canonical(artifactRoot, error);
    if (error || !std::filesystem::is_directory(artifact) || artifact == base)
        return fail("Artifact is missing or is the base distribution");
    error.clear();
    const auto outputParent =
        std::filesystem::canonical(outputPath.parent_path(), error);
    if (error || !std::filesystem::is_directory(outputParent) ||
        ExistingDirectoryIsWithin(outputParent, base) ||
        ExistingDirectoryIsWithin(outputParent, artifact))
        return fail("Overlay output parent is missing or inside the base or artifact");
    error.clear();
    if (std::filesystem::exists(outputPath, error) || error)
        return fail("Overlay output already exists or cannot be inspected");
    const bool signedOverlay = !signingKeyFile.empty();
    if (!priorUpdatesDirectory.empty() &&
        (!priorUpdatesDirectory.is_absolute() || !signedOverlay))
        return fail("Chained overlays require an absolute updates directory and a publisher signature");
    if (!optionalPackId.empty() &&
        (!ValidOptionalPackId(optionalPackId) || !signedOverlay))
        return fail("Optional overlays require a valid ID and publisher signature");

    const auto baseManifestBefore = ReadBoundedRegularFile(
        base / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto baseIndexBefore = ReadBoundedRegularFile(
        base / "Package/content-index.json", 128ull * 1024ull * 1024ull);
    if (!baseManifestBefore || !baseIndexBefore ||
        plan.manifestJson.empty() ||
        plan.manifestJson.size() > 4ull * 1024ull * 1024ull ||
        plan.contentIndexJson.empty() ||
        plan.contentIndexJson.size() > 128ull * 1024ull * 1024ull)
        return fail("A base or planned package document is unavailable or oversized");
    if (preverifiedBase &&
        (!preverifiedBase->error.empty() ||
         preverifiedBase->directory != base ||
         preverifiedBase->manifest != *baseManifestBefore ||
         preverifiedBase->index != *baseIndexBefore ||
         BaseArchiveStamps(base, *baseManifestBefore) !=
             std::optional{preverifiedBase->archives}))
        return fail(preverifiedBase->error.empty()
                        ? "Base package changed after content verification"
                        : preverifiedBase->error);
    const auto baseVerified = preverifiedBase
        ? preverifiedBase->contents
        : VerifyPackageContents(
              base, [&](const std::uint64_t current,
                        const std::uint64_t total,
                        const std::uint64_t rawBytes) {
                  if (onProgress)
                      onProgress("verify-base", current, total, rawBytes);
              });
    if (!baseVerified.valid)
        return fail("Base distribution failed content verification: " +
                    baseVerified.error);
    if (ReadBoundedRegularFile(base / "Package/manifest.json",
                               4ull * 1024ull * 1024ull) != baseManifestBefore ||
        ReadBoundedRegularFile(base / "Package/content-index.json",
                               128ull * 1024ull * 1024ull) != baseIndexBefore)
        return fail("Base package documents changed during verification");
    const auto installedBaseParsed =
        sdk::detail::ParsePackageManifest(*baseManifestBefore);
    if (!installedBaseParsed.Valid())
        return fail("Installed base manifest is invalid");
    std::string effectiveBaseManifest = *baseManifestBefore;
    std::string effectiveBaseIndex = *baseIndexBefore;
    std::optional<PackageOverlayChain> priorChain;
    if (!priorUpdatesDirectory.empty()) {
        priorChain = ResolveSignedPackageOverlayChain(
            base, priorUpdatesDirectory);
        if (!priorChain->valid || priorChain->mounts.empty() ||
            !priorChain->ignoredFiles.empty() ||
            priorChain->targetManifestJson.empty() ||
            priorChain->targetContentIndexJson.empty())
            return fail("A verified main update chain is required: " +
                        (priorChain->error.empty()
                             ? "unverified or unrelated update files"
                             : priorChain->error));
        effectiveBaseManifest = priorChain->targetManifestJson;
        effectiveBaseIndex = priorChain->targetContentIndexJson;
    }
    const auto plannedBaseManifestHash =
        FingerprintText(effectiveBaseManifest);
    if (!plannedBaseManifestHash ||
        plan.previousManifestSha256 != *plannedBaseManifestHash)
        return fail("Base manifest differs from the package planning source");
    const auto baseParsed =
        sdk::detail::ParsePackageManifest(effectiveBaseManifest);
    const auto targetParsed = sdk::detail::ParsePackageManifest(plan.manifestJson);
    if (!baseParsed.Valid() || !targetParsed.Valid() ||
        baseParsed.manifest.gameId != targetParsed.manifest.gameId ||
        baseParsed.manifest.engineVersion != targetParsed.manifest.engineVersion ||
        baseParsed.manifest.publisherPublicKey !=
            targetParsed.manifest.publisherPublicKey ||
        baseParsed.manifest.packageFingerprint ==
            targetParsed.manifest.packageFingerprint)
        return fail("Base and plan are not distinct compatible package versions");
    if (signedOverlay && baseParsed.manifest.publisherPublicKey.empty())
        return fail("Signed overlay requires a pinned publisher public key");
    if (!optionalPackId.empty() &&
        (baseParsed.manifest.contentVersion !=
             targetParsed.manifest.contentVersion ||
         baseParsed.manifest.saveVersion != targetParsed.manifest.saveVersion ||
         baseParsed.manifest.persistenceDataId !=
             targetParsed.manifest.persistenceDataId ||
         baseParsed.manifest.persistenceKeyId !=
             targetParsed.manifest.persistenceKeyId))
        return fail("Optional overlays cannot change save or content version identity");
    const auto baseEntries =
        ParseIndexedResources(effectiveBaseIndex, baseParsed.manifest);
    const auto targetEntries =
        ParseIndexedResources(plan.contentIndexJson, targetParsed.manifest);
    if (!baseEntries || !targetEntries ||
        targetEntries->size() != plan.resources.size())
        return fail("Base or planned content inventory is invalid");
    std::unordered_map<std::string, const sdk::PackagePlannedResource*> sources;
    sources.reserve(plan.resources.size());
    for (const auto& source : plan.resources) {
        const auto indexed = targetEntries->find(source.uri);
        if (indexed == targetEntries->end() ||
            indexed->second.fingerprint != source.fingerprint ||
            indexed->second.size != source.size ||
            !sources.emplace(source.uri, &source).second)
            return fail("Planned source differs from its content index: " +
                        source.uri);
    }
    if (!RuntimeInputsMatchBase(base, installedBaseParsed.manifest,
                                playerExecutable))
        return fail("Player or runtime libraries differ from the base distribution");

    std::vector<IndexedResource> changed;
    for (const auto& [uri, resource] : *targetEntries) {
        const auto old = baseEntries->find(uri);
        if (old == baseEntries->end() ||
            old->second.fingerprint != resource.fingerprint ||
            old->second.size != resource.size)
            changed.push_back(resource);
    }
    // AllInputs planning checked every artifact byte. The overlay writer only
    // opens changed sources; unchanged entries are supplied by the verified
    // base. Re-resolve the sources that can actually enter the new PDX so an
    // artifact with tens of thousands of unchanged files avoids another full
    // filesystem canonicalization pass.
    for (const auto& resource : changed) {
        const auto* source = sources.at(resource.uri);
        if (source->source.empty()) continue;
        error.clear();
        const auto canonicalSource =
            std::filesystem::canonical(source->source, error);
        if (error || !ExistingDirectoryIsWithin(
                         canonicalSource.parent_path(), artifact))
            return fail("Planned source escapes the artifact: " + resource.uri);
    }
    std::uint64_t removed = 0;
    for (const auto& [uri, resource] : *baseEntries) {
        (void)resource;
        if (!targetEntries->contains(uri)) ++removed;
    }
    std::uint64_t changedRawBytes = 0;
    for (const auto& resource : changed) {
        if (resource.size >
            (std::numeric_limits<std::uint64_t>::max)() - changedRawBytes)
            return fail("Changed resource total exceeds the overlay limit");
        changedRawBytes += resource.size;
    }
    const auto baseManifestHash = FingerprintText(effectiveBaseManifest);
    const auto baseIndexHash = FingerprintText(effectiveBaseIndex);
    const auto targetManifestHash = FingerprintText(plan.manifestJson);
    const auto targetIndexHash = FingerprintText(plan.contentIndexJson);
    if (!baseManifestHash || !baseIndexHash || !targetManifestHash ||
        !targetIndexHash)
        return fail("Overlay metadata hashing failed");
    Json metadata{
        {"format", "PrismatiXPackageOverlay"},
        {"schemaRevision", optionalPackId.empty() ? (signedOverlay ? 2 : 1) : 3},
        {"gameId", baseParsed.manifest.gameId},
        {"basePackageFingerprint", baseParsed.manifest.packageFingerprint},
        {"targetPackageFingerprint", targetParsed.manifest.packageFingerprint},
        {"baseManifestSha256", *baseManifestHash},
        {"baseContentIndexSha256", *baseIndexHash},
        {"targetManifestSha256", *targetManifestHash},
        {"targetContentIndexSha256", *targetIndexHash}};
    if (!optionalPackId.empty()) {
        metadata["kind"] = "optional";
        metadata["optionalId"] = optionalPackId;
    }
    const std::string metadataText = metadata.dump();
    if (metadataText.size() > 16ull * 1024ull)
        return fail("Overlay metadata exceeds its format limit");
    std::optional<Bytes> signature;
    if (signedOverlay) {
        auto keyText = ReadBoundedRegularFile(signingKeyFile, 256);
        if (!keyText)
            return fail("Overlay signing key file is missing or unsafe");
        const auto first = keyText->find_first_not_of(" \r\n\t");
        const auto last = keyText->find_last_not_of(" \r\n\t");
        if (first == std::string::npos)
            return fail("Overlay signing key file is empty");
        auto keyHex = keyText->substr(first, last - first + 1);
        const auto signedHex = SignOverlayMetadata(
            metadataText, keyHex, baseParsed.manifest.publisherPublicKey,
            optionalPackId.empty() ? 2 : 3);
        std::fill(keyHex.begin(), keyHex.end(), '\0');
        std::fill(keyText->begin(), keyText->end(), '\0');
        if (!signedHex)
            return fail("Overlay signing key does not match the pinned publisher key");
        signature.emplace();
        signature->reserve(64);
        for (std::size_t index = 0; index < signedHex->size(); index += 2) {
            const auto nibble = [](const char value) -> std::uint8_t {
                return value <= '9' ? value - '0' : value - 'a' + 10;
            };
            signature->push_back(static_cast<std::uint8_t>(
                (nibble((*signedHex)[index]) << 4) |
                nibble((*signedHex)[index + 1])));
        }
    }

    if (planOnly && preverifiedBase &&
        BaseArchiveStamps(base, *baseManifestBefore) !=
            std::optional{preverifiedBase->archives})
        return fail("Base package changed after content verification");
    if (planOnly)
        return {.planned = true,
                .changedResources = changed.size(),
                .removedResources = removed,
                .changedRawBytes = changedRawBytes};

    const auto stagedName =
        px::PathToUtf8(outputPath.filename()) + ".staging-" +
        baseManifestHash->substr(0, 8) + targetManifestHash->substr(0, 8);
    OwnedOverlayStage stage{outputParent / px::PathFromUtf8(stagedName)};
    error.clear();
    if (!std::filesystem::create_directory(stage.path, error) || error)
        return fail("A prior overlay staging directory exists or cannot be created");
    stage.created = true;
    ArchiveStreamWriter writer;
    const auto addText = [&writer](const std::string_view uri,
                                   const std::string& value) {
        writer.AddData(std::string(uri), Bytes(value.begin(), value.end()));
    };
    addText(kPackageOverlayManifestUri, metadataText);
    addText(kPackageOverlayTargetManifestUri, plan.manifestJson);
    addText(kPackageOverlayTargetIndexUri, plan.contentIndexJson);
    if (signature)
        writer.AddData(std::string(kPackageOverlaySignatureUri),
                       std::move(*signature));
    std::uint64_t plannedBytes = 0;
    if (onProgress) onProgress("plan-changed", 0, changed.size(), 0);
    for (std::size_t index = 0; index < changed.size(); ++index) {
        const auto& resource = changed[index];
        const auto* source = sources.at(resource.uri);
        if (!source->source.empty())
            writer.AddFile(resource.uri, source->source, resource.size,
                           resource.fingerprint,
                           resource.size >= 64ull * 1024ull);
        else
            writer.AddData(resource.uri, source->generatedData);
        if (resource.size >
            (std::numeric_limits<std::uint64_t>::max)() - plannedBytes)
            return fail("Changed resource total exceeds the overlay limit");
        plannedBytes += resource.size;
        if (onProgress)
            onProgress("plan-changed", index + 1, changed.size(), plannedBytes);
    }
    const auto stagedPdx = stage.path / "patch.pdx";
    if (!writer.Write(px::PathToUtf8(stagedPdx),
                      [&](const std::uint64_t current,
                          const std::uint64_t total) {
                          if (onProgress)
                              onProgress("write-overlay", current, total,
                                         current);
                      }))
        return fail("Overlay PDX could not be written from planned resources");
    const auto verified = priorChain
        ? VerifySignedPackageOverlayFromDocuments(
              effectiveBaseManifest, effectiveBaseIndex, stagedPdx)
        : signedOverlay ? VerifySignedPackageOverlay(base, stagedPdx)
                        : VerifyPackageOverlay(base, stagedPdx);
    if (!verified.valid || verified.changedResources != changed.size() ||
        verified.tombstones.size() != removed)
        return fail("Created overlay failed verification: " + verified.error);
    if (priorChain) {
        const auto currentChain = ResolveSignedPackageOverlayChain(
            base, priorUpdatesDirectory);
        if (!currentChain.valid || !currentChain.ignoredFiles.empty() ||
            currentChain.targetManifestJson != effectiveBaseManifest ||
            currentChain.targetContentIndexJson != effectiveBaseIndex ||
            currentChain.mounts.size() != priorChain->mounts.size() ||
            !std::ranges::equal(
                currentChain.mounts, priorChain->mounts,
                [](const auto& left, const auto& right) {
                    return left.path == right.path;
                }))
            return fail("Signed update chain changed during overlay creation");
    }
    if (!RuntimeInputsMatchBase(base, installedBaseParsed.manifest,
                                playerExecutable) ||
        (preverifiedBase &&
         BaseArchiveStamps(base, *baseManifestBefore) !=
             std::optional{preverifiedBase->archives}) ||
        ReadBoundedRegularFile(base / "Package/manifest.json",
                               4ull * 1024ull * 1024ull) != baseManifestBefore ||
        ReadBoundedRegularFile(base / "Package/content-index.json",
                               128ull * 1024ull * 1024ull) != baseIndexBefore)
        return fail("Base runtime or metadata changed during overlay creation");
#ifdef _WIN32
    if (!MoveFileExW(stagedPdx.c_str(), outputPath.c_str(),
                     MOVEFILE_WRITE_THROUGH))
        return fail("Overlay could not be published without replacing existing data: "
                    "Win32 error " + std::to_string(GetLastError()));
#else
    error.clear();
    std::filesystem::create_hard_link(stagedPdx, outputPath, error);
    if (error)
        return fail("Overlay could not be published without replacing existing data: " +
                    error.message());
#endif
    return {.created = true,
            .changedResources = verified.changedResources,
            .removedResources = removed,
            .changedRawBytes = verified.changedRawBytes};
}

PackageOverlayBuildResult CreatePackageOverlayFromPlan(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& artifactRoot,
    const sdk::PackagePlanResult& plan,
    const std::filesystem::path& playerExecutable,
    const std::filesystem::path& outputPath,
    const PackageOverlayBuildProgress& onProgress,
    const std::filesystem::path& signingKeyFile,
    const std::string_view optionalPackId,
    const std::filesystem::path& priorUpdatesDirectory,
    const bool planOnly) {
    return CreatePackageOverlayFromPlanImpl(
        baseDistribution, artifactRoot, plan, playerExecutable, outputPath,
        onProgress, signingKeyFile, optionalPackId, priorUpdatesDirectory,
        planOnly, nullptr);
}

PackageOverlayBuildResult CreatePackageOverlayFromRequest(
    const std::filesystem::path& baseDistribution,
    const sdk::PackageRequest& request,
    const std::filesystem::path& outputPath,
    const sdk::PackageEventSink& onPackageEvent,
    const PackageOverlayBuildProgress& onProgress,
    const std::filesystem::path& signingKeyFile,
    const std::string_view optionalPackId,
    const std::filesystem::path& priorUpdatesDirectory,
    const std::string_view previousManifestJson,
    const bool planOnly) {
    std::mutex callbackMutex;
    const auto serializedProgress = [&](const std::string_view phase,
                                        const std::uint64_t current,
                                        const std::uint64_t total,
                                        const std::uint64_t rawBytes) {
        if (!onProgress) return;
        std::lock_guard lock(callbackMutex);
        onProgress(phase, current, total, rawBytes);
    };
    const auto serializedEvent = [&](const sdk::PackageEvent& event) {
        if (!onPackageEvent) return;
        std::lock_guard lock(callbackMutex);
        onPackageEvent(event);
    };
#ifdef _WIN32
    // Holding the base archives with FILE_SHARE_READ keeps their bytes fixed
    // while the independent artifact sources are hashed on the main thread.
    std::stop_source stopBase;
    auto baseFuture = std::async(std::launch::async, [&] {
        return VerifyBaseForOverlay(baseDistribution, serializedProgress,
                                    stopBase.get_token());
    });
#endif
    auto plan = sdk::PlanPackageRequest(
        request, serializedEvent, sdk::PackageValidationDepth::AllInputs,
        previousManifestJson);
#ifdef _WIN32
    if (!plan.valid) stopBase.request_stop();
    auto base = baseFuture.get();
#else
    // POSIX advisory locks cannot prevent another writer from replacing the
    // archive. Keep the original verification order on those platforms.
    auto base = VerifyBaseForOverlay(baseDistribution, serializedProgress);
#endif
    if (!plan.valid) {
        PackageOverlayBuildResult result;
        result.error = plan.diagnostics.empty()
            ? "Package planning failed"
            : plan.diagnostics.front().message;
        result.diagnostics = std::move(plan.diagnostics);
        return result;
    }
    if (!base.error.empty()) return {.error = std::move(base.error)};
    return CreatePackageOverlayFromPlanImpl(
        baseDistribution, request.projectRoot, plan, request.playerExecutable,
        outputPath, serializedProgress, signingKeyFile, optionalPackId,
        priorUpdatesDirectory, planOnly, &base);
}

}  // namespace px::io
