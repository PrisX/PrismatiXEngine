#include "Editor/Assets/AssetDatabase.h"

#include <cctype>
#include <unordered_set>

namespace px::editor {

namespace {
std::string ToForward(std::filesystem::path p) {
    const std::u8string value = p.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string CanonicalKey(const std::filesystem::path& path, std::error_code& error) {
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) return {};
    std::string value = ToForward(canonical);
#ifdef _WIN32
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
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
}

std::string AssetDatabase::Classify(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp")
        return "image";
    if (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".opus")
        return "audio";
    if (ext == ".pds") return "script";
    if (ext == ".pxscene") return "ui";
    if (ext == ".pxres" || ext == ".pxtheme" || ext == ".pxanim") return "resource";
    if (ext == ".ttf" || ext == ".otf") return "font";
    if (ext == ".lua") return "lua";
    return "other";
}

void AssetDatabase::Scan(const ProjectContext& context) {
    ++m_revision;
    m_assets.clear();
    const std::filesystem::path data = context.DataRoot();
    if (!std::filesystem::exists(data)) {
        Log("No Content/ folder to scan.");
        return;
    }
    std::error_code ec;
    std::unordered_set<std::string> visitedDirectories;
    visitedDirectories.insert(CanonicalKey(data, ec));
    ec.clear();
    std::size_t skippedDirectories = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             data, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            const auto status = it->symlink_status(ec);
            const auto canonical = std::filesystem::weakly_canonical(it->path(), ec);
            const std::string key = CanonicalKey(canonical, ec);
            const bool repeat = !key.empty() && !visitedDirectories.insert(key).second;
            if (ec || std::filesystem::is_symlink(status) || repeat ||
                !IsWithin(canonical, data, ec)) {
                it.disable_recursion_pending();
                ++skippedDirectories;
            }
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path& abs = it->path();
        const std::string ext = abs.extension().string();
        if (ext == ".pxmeta") continue;
        AssetRecord rec;
        rec.absolutePath = abs;
        rec.runtimePath = ToForward(std::filesystem::relative(abs, context.root, ec));
        rec.type = Classify(abs);
        rec.size = std::filesystem::file_size(abs, ec);
        m_assets.push_back(std::move(rec));
    }
    Log("Scanned " + std::to_string(m_assets.size()) + " assets.");
    if (skippedDirectories)
        Log("Skipped " + std::to_string(skippedDirectories) +
            " linked or cyclic Content directories.");
}

std::vector<AssetRecord> AssetDatabase::Filter(std::string_view text, std::string_view type) const {
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(std::string(text));
    std::vector<AssetRecord> out;
    for (const AssetRecord& r : m_assets) {
        if (!type.empty() && type != "all" && r.type != type) continue;
        if (!needle.empty() && lower(r.runtimePath).find(needle) == std::string::npos) continue;
        out.push_back(r);
    }
    return out;
}

}
