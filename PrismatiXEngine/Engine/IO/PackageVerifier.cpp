#include "Engine/IO/PackageVerifier.h"

#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/VFS.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/Support/Utf8Path.h"

#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace px::io {
namespace {

using Json = nlohmann::json;

struct IndexedResource {
    std::string uri;
    std::string fingerprint;
    std::uint64_t size = 0;
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

bool LowerFingerprint(const std::string_view value) {
    return value.size() == 64 &&
        std::ranges::all_of(value, [](const unsigned char byte) {
            return std::isdigit(byte) || (byte >= 'a' && byte <= 'f');
        });
}

std::optional<std::string> FingerprintStream(SeekableReadStream& stream) {
    if (psa_crypto_init() != PSA_SUCCESS) return std::nullopt;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return std::nullopt;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t remaining = stream.Size();
    while (remaining > 0) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        const auto received = stream.Read(buffer.data(), requested);
        if (received == 0 || received > requested || stream.Failed() ||
            psa_hash_update(&operation, buffer.data(), received) != PSA_SUCCESS) {
            psa_hash_abort(&operation);
            return std::nullopt;
        }
        remaining -= received;
    }
    if (stream.Failed()) {
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

}  // namespace

PackageContentVerification VerifyPackageContents(
    const std::filesystem::path& distribution,
    const PackageVerificationProgress& onProgress) {
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
    PackageContentVerification result;
    const auto resourceTotal =
        static_cast<std::uint64_t>(index["entries"].size());
    if (onProgress) onProgress(0, resourceTotal, 0);
    std::uint64_t lastReportedBytes = 0;
    for (const auto& archiveMount : manifest.archives) {
        const auto path = distribution / px::PathFromUtf8(archiveMount.file);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
            return Fail("Mounted PDX is missing or not a regular file: " + archiveMount.file);
        Archive archive;
        if (!archive.Open(px::PathToUtf8(path), key ? &*key : nullptr))
            return Fail("Mounted PDX cannot be opened: " + archiveMount.file);
        const auto& resources = expected.at(archiveMount.file);
        if (archive.Entries().size() != resources.size())
            return Fail("PDX entry count differs from content index: " + archiveMount.file);
        std::set<std::string> actualNames;
        for (const auto& entry : archive.Entries())
            if (!actualNames.insert(entry.name).second)
                return Fail("PDX contains a duplicate resource: " + entry.name);
        for (const auto& resource : resources) {
            if (!actualNames.contains(resource.uri))
                return Fail("Indexed resource is missing from PDX: " + resource.uri);
            auto stream = archive.OpenStream(resource.uri);
            if (!stream || stream->Size() != resource.size)
                return Fail("Indexed resource size differs from PDX: " + resource.uri);
            const auto fingerprint = FingerprintStream(*stream);
            if (!fingerprint || *fingerprint != resource.fingerprint)
                return Fail("Indexed resource SHA-256 differs from PDX: " + resource.uri);
            result.resources += 1;
            result.rawBytes += resource.size;
            if (onProgress &&
                (result.resources == resourceTotal ||
                 result.resources % 256 == 0 ||
                 result.rawBytes - lastReportedBytes >= 64ull * 1024ull * 1024ull)) {
                onProgress(result.resources, resourceTotal, result.rawBytes);
                lastReportedBytes = result.rawBytes;
            }
        }
        result.archives += 1;
    }
    result.valid = true;
    return result;
}

}  // namespace px::io
