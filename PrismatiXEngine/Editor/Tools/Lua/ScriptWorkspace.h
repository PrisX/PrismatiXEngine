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
    struct ActionParameterDraft {
        std::string name = "value";
        std::string displayName = "Value";
        std::string type = "string";
        std::string defaultValue;
        std::string enumValues;
        std::string resourceFilter;
        std::string editorHint = "default";
        float minimum = 0.0f;
        float maximum = 1.0f;
        bool required = false;
        bool hasRange = false;
    };
    void RefreshFiles();
    void ScanCommands();
    void LoadFile(const std::string& runtimePath);
    bool SaveCurrent();
    bool CloseFile(const std::string& runtimePath, bool save);
    void RenderFileList();
    void RenderEditor();
    void RenderCommandList();
    void RenderApiReference();
    void RenderActionWizard();
    bool CreateLuaAction();
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
    bool m_actionWizardOpen = false;
    std::string m_actionManifest = "Content/Extensions/default.pxextension";
    std::string m_actionId = "game.customAction";
    std::string m_actionDisplayName = "Custom Action";
    std::string m_actionDescription;
    std::string m_actionCategory = "Game";
    int m_actionReentry = 0;
    bool m_actionRuntime = true;
    bool m_actionUi = true;
    bool m_actionAnimation = false;
    bool m_actionAudio = false;
    std::vector<ActionParameterDraft> m_actionParameters;
};

}
