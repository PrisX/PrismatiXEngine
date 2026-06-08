#pragma once

#include "Editor/Build/BuildService.h"
#include "Editor/Assets/AssetDatabase.h"
#include "Editor/Assets/EditorTextures.h"
#include "Editor/Tools/Database/DatabasePanel.h"
#include "Editor/Tools/Flow/FlowMap.h"
#include "Engine/Project/Database.h"
#include "Editor/Tools/NodeEditor/NodeGraphEditor.h"
#include "Editor/Preview/RuntimeHost.h"
#include "Editor/Tools/Lua/ScriptWorkspace.h"
#include "Editor/Assets/AssetMeta.h"
#include "Editor/Project/ProjectService.h"
#include "Editor/Tools/UIDesigner/UIDesigner.h"
#include "Editor/Workspace/DocumentRegistry.h"
#include "Editor/Workspace/UndoStack.h"
#include "Engine/Platform/Window.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace px::editor {

class EditorApp {
public:
    EditorApp();

    bool Init();
    void Run();
    void Shutdown();
    void OpenWorkspace();

private:
    void BuildUI();
    void SyncDesigner();
    void ApplyTheme();
    void LoadFonts();
    void BuildDockLayout(unsigned int dockspaceId);
    void RenderWelcome();
    void RenderMenuBar();
    void RenderHierarchy();
    void RenderInspector();
    void RenderAssets();
    void RenderConsole();
    void RenderPreview();
    void RenderNodeEditor();
    void RenderPDSText();
    void RenderBuild();
    void RenderDatabase();
    void RenderAnimation();
    void RenderFlow();
    void RenderScripting();
    void RenderProblems();
    void RenderCommandPalette();
    void RenderShortcutsWindow();
    void BuildCommands();
    void HandleShortcuts();
    void RefreshProblems();

    void OpenProject(const std::filesystem::path& root);
    void RunBuild();
    void RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir);
    void RunDev();
    void RunPackaged();
    void OpenInExplorer(const std::filesystem::path& path);
    void SaveAll();
    void CreateFlowChapter(ImVec2 canvasPosition);
    void Log(const std::string& message);

    px::Window m_window;
    ProjectService m_project;
    AssetDatabase m_assets;
    DocumentRegistry m_docs;
    UndoStack m_undo;

    std::unique_ptr<RuntimeHost> m_preview;
    std::unique_ptr<EditorTextures> m_textures;
    ImTextureID m_nodeHeaderTex{};
    int m_nodeHeaderW = 0;
    int m_nodeHeaderH = 0;
    UIDesigner m_designer;
    std::string m_designerPath;
    char m_newScreenName[96] = "new_screen";
    NodeGraphEditor m_nodeEditor{ NodeGraphEditor::GraphKind::PDSDialogue };
    px::project::Database m_database;
    DatabasePanel m_dbPanel;
    std::string m_dbPath;
    bool m_dbDirty = false;
    bool m_previewAnims = false;
    FlowMap m_flow;
    ScriptWorkspace m_scripts;

    std::vector<std::string> m_console;
    char m_assetFilter[128] = { 0 };
    int m_assetTypeIndex = 0;
    std::string m_selectedAsset;
    std::string m_metaAsset;
    AssetImportSettings m_meta;
    int m_previewMode = 0;
    int m_buildProfile = 1;
    bool m_running = false;
    bool m_imguiReady = false;

    struct PaletteCommand {
        std::string label;
        std::string shortcut;
        std::function<void()> run;
    };
    std::vector<PaletteCommand> m_commands;
    bool m_paletteOpen = false;
    bool m_paletteFocus = false;
    bool m_showShortcuts = false;
    char m_paletteFilter[128] = { 0 };
    std::vector<std::string> m_problems;

    enum class Screen { Welcome, Workspace };
    Screen m_screen = Screen::Welcome;
    bool m_buildLayout = false;
    char m_openPath[512] = { 0 };
    char m_newName[128] = { 0 };
    char m_newPath[512] = { 0 };
    std::string m_basePath;
    std::string m_iniPath;
};

}
