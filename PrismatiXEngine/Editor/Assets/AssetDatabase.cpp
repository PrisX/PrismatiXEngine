#include "Editor/Assets/AssetDatabase.h"

#include <cctype>

namespace px::editor {

namespace {
std::string ToForward(std::filesystem::path p) {
    return p.generic_string();
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
    if (ext == ".pxui") return "ui";
    if (ext == ".ttf" || ext == ".otf") return "font";
    if (ext == ".lua") return "lua";
    return "other";
}

void AssetDatabase::Scan(const ProjectContext& context) {
    m_assets.clear();
    const std::filesystem::path data = context.DataRoot();
    if (!std::filesystem::exists(data)) {
        Log("No Data/ folder to scan.");
        return;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(data, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path& abs = it->path();
        const std::string ext = abs.extension().string();
        if (ext == ".meta") continue;
        AssetRecord rec;
        rec.absolutePath = abs;
        rec.runtimePath = ToForward(std::filesystem::relative(abs, context.root, ec));
        rec.type = Classify(abs);
        rec.size = std::filesystem::file_size(abs, ec);
        m_assets.push_back(std::move(rec));
    }
    Log("Scanned " + std::to_string(m_assets.size()) + " assets.");
}

std::vector<AssetRecord> AssetDatabase::Filter(std::string_view text, std::string_view type) const {
    std::vector<AssetRecord> out;
    for (const AssetRecord& r : m_assets) {
        if (!type.empty() && type != "all" && r.type != type) continue;
        if (!text.empty() && r.runtimePath.find(text) == std::string::npos) continue;
        out.push_back(r);
    }
    return out;
}

}
