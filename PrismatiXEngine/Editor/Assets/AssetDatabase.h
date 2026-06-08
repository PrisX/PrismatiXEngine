#pragma once

#include "Editor/Project/ProjectTypes.h"

#include <string_view>

namespace px::editor {

struct AssetRecord {
    std::string runtimePath;
    std::filesystem::path absolutePath;
    std::string type;
    std::uintmax_t size = 0;
};

class AssetDatabase {
public:
    explicit AssetDatabase(LogSink log = {}) : m_log(std::move(log)) {}

    void Scan(const ProjectContext& context);
    [[nodiscard]] const std::vector<AssetRecord>& Assets() const { return m_assets; }
    [[nodiscard]] std::vector<AssetRecord> Filter(std::string_view text,
                                                  std::string_view type) const;
    [[nodiscard]] static std::string Classify(const std::filesystem::path& path);

private:
    void Log(const std::string& msg) const {
        if (m_log) m_log(msg);
    }

    LogSink m_log;
    std::vector<AssetRecord> m_assets;
};

}
