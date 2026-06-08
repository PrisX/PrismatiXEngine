#pragma once

#include "Editor/Scripting/CustomCommand.h"
#include "Editor/Services/EditorServices.h"

#include <functional>
#include <string>
#include <vector>

namespace px::editor {

class ScriptWorkspace {
public:
    explicit ScriptWorkspace(LogSink log = {}) : m_log(std::move(log)) {}

    void SetProject(const ProjectContext* context);
    void SetOnCommandsChanged(std::function<void(const std::vector<CustomCommandDef>&)> cb) {
        m_onCommands = std::move(cb);
    }
    void Render();
    void Rescan();

    [[nodiscard]] const std::vector<CustomCommandDef>& Commands() const { return m_commands; }

private:
    void RefreshFiles();
    void ScanCommands();
    void LoadFile(const std::string& runtimePath);
    void SaveCurrent();
    void RenderFileList();
    void RenderEditor();
    void RenderCommandList();
    void RenderApiReference();
    void Log(const std::string& msg) const {
        if (m_log) m_log(msg);
    }

    LogSink m_log;
    const ProjectContext* m_project = nullptr;

    std::vector<std::string> m_files;
    std::string m_currentFile;
    std::string m_buffer;
    bool m_dirty = false;

    std::vector<CustomCommandDef> m_commands;
    std::function<void(const std::vector<CustomCommandDef>&)> m_onCommands;

    char m_newName[96] = "extensions";
    std::string m_status;
};

}
