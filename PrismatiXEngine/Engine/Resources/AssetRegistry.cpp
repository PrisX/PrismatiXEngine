#include "Engine/Resources/AssetRegistry.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Resources/TypedDocument.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace px::resource {

namespace {
std::string NormalPath(const std::filesystem::path& path) {
    const std::u8string encoded = path.lexically_normal().generic_u8string();
    std::string value(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#ifdef _WIN32
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return value;
}

bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& root,
              std::error_code& error) {
    const auto relative = std::filesystem::relative(path, root, error);
    if (error) return false;
    for (const auto& part : relative) if (part == "..") return false;
    return true;
}

std::string FnvHash(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::uint64_t hash = 1469598103934665603ull;
    char buffer[8192];
    while (in) {
        in.read(buffer, sizeof(buffer));
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

diag::Diagnostic AssetError(std::string code, const std::filesystem::path& path,
                            std::string message, std::string details = {},
                            std::string quickFix = {}) {
    diag::Diagnostic d;
    d.severity = diag::Severity::Error;
    d.code = std::move(code);
    d.category = "asset-registry";
    d.message = std::move(message);
    d.details = std::move(details);
    d.source.path = NormalPath(path);
    d.quickFix = std::move(quickFix);
    return d;
}
}

std::filesystem::path AssetRegistry::MetaPath(const std::filesystem::path& asset) {
    std::filesystem::path meta = asset;
    meta += std::filesystem::path(".pxmeta");
    return meta;
}

Status AssetRegistry::Scan(const std::filesystem::path& projectRoot) {
    m_projectRoot = projectRoot;
    m_entries.clear();
    m_diagnostics.clear();
    m_byId.clear();
    m_byPath.clear();
    const std::filesystem::path content = projectRoot / "Content";
    if (!std::filesystem::exists(content)) {
        m_diagnostics.push_back(AssetError("PXASSET-E1001", content,
                                           "The project Content directory does not exist."));
        return Status::Fail(m_diagnostics);
    }
    std::error_code ec;
    std::unordered_set<std::string> visitedDirectories;
    visitedDirectories.insert(NormalPath(std::filesystem::weakly_canonical(content, ec)));
    ec.clear();
    std::size_t skippedDirectories = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             content, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            const auto status = it->symlink_status(ec);
            const auto canonical = std::filesystem::weakly_canonical(it->path(), ec);
            const std::string key = NormalPath(canonical);
            const bool repeat = !key.empty() && !visitedDirectories.insert(key).second;
            if (ec || std::filesystem::is_symlink(status) || repeat ||
                !IsWithin(canonical, content, ec)) {
                it.disable_recursion_pending();
                ++skippedDirectories;
            }
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const auto source = it->path();
        if (source.extension() == ".pxmeta") continue;
        const auto meta = MetaPath(source);
        if (!std::filesystem::exists(meta)) {
            m_diagnostics.push_back(AssetError("PXASSET-E1002", source,
                                               "The asset has no identity metadata.",
                                               "Register the asset in the Content browser.",
                                               "asset.register"));
            continue;
        }
        auto loaded = LoadMeta(source, meta);
        if (!loaded) {
            m_diagnostics.insert(m_diagnostics.end(), loaded.Diagnostics().begin(),
                                 loaded.Diagnostics().end());
            continue;
        }
        m_entries.push_back(loaded.TakeValue());
    }
    if (ec) m_diagnostics.push_back(AssetError("PXASSET-E1003", content, "Asset scan failed.", ec.message()));
    if (skippedDirectories) {
        diag::Diagnostic warning{.severity = diag::Severity::Warning,
            .code = "PXASSET-W1009", .category = "asset-registry",
            .message = "Linked or cyclic Content directories were skipped",
            .details = std::to_string(skippedDirectories) +
                " directory entries were not followed to keep scanning safe."};
        warning.source.path = NormalPath(content);
        m_diagnostics.push_back(std::move(warning));
    }

    RebuildIndexes();
    for (const diag::Diagnostic& d : m_diagnostics) diag::Emit(d);
    return Valid() ? Status::Ok() : Status::Fail(m_diagnostics);
}

Result<AssetEntry> AssetRegistry::RegisterAsset(const std::filesystem::path& projectRoot,
                                                const std::filesystem::path& assetPath,
                                                std::string type) {
    if (!std::filesystem::exists(assetPath)) {
        return Result<AssetEntry>::Failure(
            AssetError("PXASSET-E1004", assetPath, "Cannot register a missing asset."));
    }
    m_projectRoot = projectRoot;
    if (const AssetEntry* registered = FindPath(assetPath)) {
        return Result<AssetEntry>::Success(*registered);
    }
    const std::filesystem::path metaPath = MetaPath(assetPath);
    if (std::filesystem::exists(metaPath)) {
        auto existing = LoadMeta(assetPath, metaPath);
        if (!existing) return existing;
        if (const auto duplicate = m_byId.find(existing.Value().id); duplicate != m_byId.end()) {
            return Result<AssetEntry>::Failure(AssetError(
                "PXASSET-E1007", assetPath, "Duplicate asset GUID blocks registration.",
                "Also used by " + m_entries[duplicate->second].sourcePath.generic_string(),
                "asset.resolve_identity"));
        }
        const std::size_t index = m_entries.size();
        m_entries.push_back(existing.Value());
        m_byId.emplace(existing.Value().id, index);
        m_byPath.emplace(NormalPath(assetPath), index);
        return Result<AssetEntry>::Success(existing.Value());
    }
    AssetEntry entry;
    entry.id = Uuid::Random();
    entry.sourcePath = assetPath;
    entry.metaPath = metaPath;
    entry.type = type.empty() ? assetPath.extension().string() : std::move(type);
    entry.sourceHash = FnvHash(assetPath);
    Status saved = SaveMeta(entry);
    if (!saved) return Result<AssetEntry>::Failure(saved.Diagnostics());
    const std::size_t index = m_entries.size();
    m_entries.push_back(entry);
    m_byId.emplace(entry.id, index);
    m_byPath.emplace(NormalPath(assetPath), index);
    return Result<AssetEntry>::Success(std::move(entry));
}

Result<Uuid> AssetRegistry::ReassignIdentity(const std::filesystem::path& sourcePath) {
    AssetEntry* entry = nullptr;
    for (AssetEntry& candidate : m_entries)
        if (NormalPath(candidate.sourcePath) == NormalPath(sourcePath)) entry = &candidate;
    if (!entry) return Result<Uuid>::Failure(AssetError("PXASSET-E1005", sourcePath, "Asset is not registered."));
    entry->id = Uuid::Random();
    Status saved = SaveMeta(*entry);
    if (!saved) return Result<Uuid>::Failure(saved.Diagnostics());
    const Uuid id = entry->id;
    RebuildIndexes();
    return Result<Uuid>::Success(id);
}

Status AssetRegistry::SetIncludeInBuild(const std::filesystem::path& sourcePath, bool include) {
    const auto found = m_byPath.find(NormalPath(sourcePath));
    if (found != m_byPath.end()) {
        AssetEntry& entry = m_entries[found->second];
        if (entry.includeInBuild == include) return Status::Ok();
        entry.includeInBuild = include;
        return SaveMeta(entry);
    }
    return Status::Fail(AssetError("PXASSET-E1008", sourcePath,
                                  "Cannot change build inclusion for an unregistered asset."));
}

const AssetEntry* AssetRegistry::Find(const Uuid& id) const {
    auto it = m_byId.find(id);
    return it == m_byId.end() ? nullptr : &m_entries[it->second];
}
const AssetEntry* AssetRegistry::FindPath(const std::filesystem::path& path) const {
    auto it = m_byPath.find(NormalPath(path));
    return it == m_byPath.end() ? nullptr : &m_entries[it->second];
}

bool AssetRegistry::Valid() const {
    for (const auto& d : m_diagnostics)
        if (d.BlocksBuild()) return false;
    return true;
}

Result<AssetEntry> AssetRegistry::LoadMeta(const std::filesystem::path& source,
                                           const std::filesystem::path& meta) const {
    std::ifstream in(meta, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    auto document = ParseTypedDocument(text.str(), meta.generic_string());
    if (!document) return Result<AssetEntry>::Failure(document.Diagnostics());
    if (document.Value().kind != DocumentKind::Meta) {
        return Result<AssetEntry>::Failure(AssetError("PXASSET-E1006", meta, "File is not pxmeta."));
    }
    AssetEntry entry;
    entry.id = document.Value().id;
    entry.sourcePath = source;
    entry.metaPath = meta;
    if (auto it = document.Value().properties.find("type"); it != document.Value().properties.end())
        if (const auto* value = it->second.TryGet<std::string>()) entry.type = *value;
    if (auto it = document.Value().properties.find("source_hash"); it != document.Value().properties.end())
        if (const auto* value = it->second.TryGet<std::string>()) entry.sourceHash = *value;
    if (auto it = document.Value().properties.find("include_in_build"); it != document.Value().properties.end())
        if (const auto* value = it->second.TryGet<bool>()) entry.includeInBuild = *value;
    return Result<AssetEntry>::Success(std::move(entry));
}

Status AssetRegistry::SaveMeta(const AssetEntry& entry) const {
    TypedDocument document;
    document.kind = DocumentKind::Meta;
    document.id = entry.id;
    document.type = "AssetMeta";
    document.properties["include_in_build"] = entry.includeInBuild;
    document.properties["source_hash"] = entry.sourceHash;
    document.properties["type"] = entry.type;
    Status status = io::AtomicFile::WriteText(entry.metaPath, WriteTypedDocument(document));
    for (const auto& d : status.Diagnostics()) diag::Emit(d);
    return status;
}

void AssetRegistry::RebuildIndexes() {
    m_byId.clear();
    m_byPath.clear();
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const AssetEntry& entry = m_entries[i];
        auto [idIt, inserted] = m_byId.emplace(entry.id, i);
        if (!inserted) {
            diag::Diagnostic d = AssetError(
                "PXASSET-E1007", entry.sourcePath, "Duplicate asset GUID blocks loading and build.",
                "Also used by " + m_entries[idIt->second].sourcePath.generic_string(),
                "asset.resolve_identity");
            d.source.resourceId = entry.id.ToString();
            m_diagnostics.push_back(std::move(d));
        }
        m_byPath.emplace(NormalPath(entry.sourcePath), i);
    }
}

}  // namespace px::resource
