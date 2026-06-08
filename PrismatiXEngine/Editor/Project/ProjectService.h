#pragma once

#include "Editor/Project/ProjectTypes.h"

namespace px::editor {

class ProjectService {
public:
    explicit ProjectService(LogSink log = {}) : m_log(std::move(log)) {}

    [[nodiscard]] ProjectContext& Context() { return m_context; }
    [[nodiscard]] const ProjectContext& Context() const { return m_context; }

    bool Open(const std::filesystem::path& root);
    bool Create(const std::filesystem::path& root, const std::string& name,
                const std::filesystem::path& fontSource = {});
    bool SaveManifest() const;

private:
    void Log(const std::string& msg) const {
        if (m_log) m_log(msg);
    }

    LogSink m_log;
    ProjectContext m_context;
};

}
