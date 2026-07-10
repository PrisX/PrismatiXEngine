#pragma once

#include "Editor/Tools/Lua/CustomCommand.h"
#include "Editor/Project/ProjectTypes.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::editor {

class ScriptWorkspace {
public:
    struct DocumentSession { std::string runtimePath; std::string buffer; bool dirty=false; };
    explicit ScriptWorkspace(LogSink log = {}) : m_log(std::move(log)) {}

    void SetProject(const ProjectContext* context);
    void SetOnCommandsChanged(std::function<void(const std::vector<CustomCommandDef>&)> cb) {
        m_onCommands = std::move(cb);
    }
    void Render();
    void Rescan();
    void OpenFile(const std::string& runtimePath) { LoadFile(runtimePath); }
    bool ReloadFile(const std::string& runtimePath);
    void SaveAll();
    [[nodiscard]] std::vector<DocumentSession> OpenDocuments() const;

    [[nodiscard]] const std::vector<CustomCommandDef>& Commands() const { return m_commands; }

private:
    void RefreshFiles();
    void ScanCommands();
    void LoadFile(const std::string& runtimePath);
    bool SaveCurrent();
    bool CloseFile(const std::string& runtimePath, bool save);
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
    std::unordered_map<std::string, DocumentSession> m_inactiveDocuments;
    std::string m_closeRequest;
    bool m_closePopup = false;

    std::vector<CustomCommandDef> m_commands;
    std::function<void(const std::vector<CustomCommandDef>&)> m_onCommands;

    char m_newName[96] = "extensions";
    std::string m_status;
};

}
