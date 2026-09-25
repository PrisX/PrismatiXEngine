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
    std::string archive;
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

PackageOverlayVerification VerifyPackageOverlay(
    const std::filesystem::path& distribution,
    const std::filesystem::path& overlayPath) {
    const auto fail = [](std::string message) {
        return PackageOverlayVerification{.error = std::move(message)};
    };
    const auto baseManifestText = ReadBoundedRegularFile(
        distribution / "Package/manifest.json", 4ull * 1024ull * 1024ull);
    const auto baseIndexText = ReadBoundedRegularFile(
        distribution / "Package/content-index.json",
        128ull * 1024ull * 1024ull);
    if (!baseManifestText || !baseIndexText)
        return fail("Base package manifest or content index is unavailable");
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
    static const std::set<std::string> requiredFields{
        "format", "schemaRevision", "gameId", "basePackageFingerprint",
        "targetPackageFingerprint", "baseManifestSha256",
        "baseContentIndexSha256", "targetManifestSha256",
        "targetContentIndexSha256"};
    if (!metadata.is_object() || metadata.size() != requiredFields.size() ||
        !metadata.contains("schemaRevision") ||
        !metadata["schemaRevision"].is_number_integer() ||
        metadata["schemaRevision"] != 1)
        return fail("Overlay metadata schema is invalid");
    for (const auto& field : requiredFields) {
        if (!metadata.contains(field) ||
            (field != "schemaRevision" && !metadata[field].is_string()))
            return fail("Overlay metadata contains a missing or invalid field");
    }
    if (metadata["format"] != "PrismatiXPackageOverlay" ||
        metadata["gameId"] != baseParsed.manifest.gameId ||
        metadata["basePackageFingerprint"] !=
            baseParsed.manifest.packageFingerprint)
        return fail("Overlay targets a different game or base package");
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
        targetParsed.manifest.packageFingerprint ==
            baseParsed.manifest.packageFingerprint ||
        metadata["targetPackageFingerprint"] !=
            targetParsed.manifest.packageFingerprint)
        return fail("Overlay target manifest is incompatible or invalid");
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
    result.valid = true;
    return result;
}

}  // namespace px::io
