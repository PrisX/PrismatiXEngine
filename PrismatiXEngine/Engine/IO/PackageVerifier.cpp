#include "Engine/IO/PackageVerifier.h"

#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/OverlaySignature.h"
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

bool ValidOptionalPackId(const std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
        std::ranges::all_of(value, [](const unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                   (byte >= '0' && byte <= '9') || byte == '-';
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
    const std::string_view enabledOptionalPackId) {
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
    if (!result.mounts.empty())
        result.targetManifestJson = *currentManifest;
    const auto main = sdk::detail::ParsePackageManifest(*currentManifest);
    if (!main.Valid()) return fail("Signed update chain target is invalid");
    result.mainPackageFingerprint = main.manifest.packageFingerprint;
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
            resource.size, resource.fingerprint);
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

}  // namespace px::io
