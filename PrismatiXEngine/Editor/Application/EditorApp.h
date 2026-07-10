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
#include <filesystem>
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
    void RenderAssetTree(const std::filesystem::path& dir, const std::filesystem::path& root);
    void RenderAssetEntry(const AssetRecord& rec, bool gridMode, float tile);
    void OpenAssetByType(const AssetRecord& rec);
    void MoveAssetTo(const std::string& runtimePath, const std::filesystem::path& targetDir);
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
    void RenderLocalization();
    void LocScanScripts();
    void LocLoad();
    void LocSave();
    void LocExportCsv();
    void RenderCommandPalette();
    void RenderShortcutsWindow();
    void BuildCommands();
    void HandleShortcuts();
    void RefreshProblems();

    void OpenProject(const std::filesystem::path& root);
    // Rewrites references to a renamed/moved asset in scripts, UI screens, the
    // database, and the manifest. Returns the number of files touched.
    int UpdateAssetReferences(const std::string& oldRel, const std::string& newRel);
    [[nodiscard]] const std::vector<std::string>& ScriptFileNames();
    void CreateScriptFile(const std::string& script);
    void AddJumpToScript(const std::string& fromScript, const std::string& toScript);
    void RemoveJumpFromScript(const std::string& fromScript, const std::string& toScript);
    void ApplyDatabaseSnapshot(const std::string& json);
    void RecordDatabaseUndo(const std::string& label, std::string before, std::string after);
    void LoadRecentProjects();
    void AddRecentProject(const std::filesystem::path& root);
    void RunBuild();
    void RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir);
    void RunDev();
    void RunPackaged();
    void OpenInExplorer(const std::filesystem::path& path);
    void ImportAssetFiles(const std::vector<std::filesystem::path>& paths);
    void ImportClipboardAssets();
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

    // Open PDS documents, one NodeGraphEditor per tab.
    std::vector<std::unique_ptr<NodeGraphEditor>> m_scriptDocs;
    int m_activeDoc = -1;
    int m_focusDocRequest = -1;
    std::vector<CustomCommandDef> m_customCommands;
    [[nodiscard]] NodeGraphEditor* ActiveDocPtr();
    NodeGraphEditor* OpenDocTab(const std::string& runtimePath);
    void ConfigureDoc(NodeGraphEditor& doc);

    px::project::Database m_database;
    DatabasePanel m_dbPanel;
    std::string m_dbPath;
    bool m_dbDirty = false;
    bool m_flowStale = false;
    bool m_previewAnims = false;
    FlowMap m_flow;
    ScriptWorkspace m_scripts;

    std::vector<std::string> m_console;
    char m_assetFilter[128] = { 0 };
    int m_assetTypeIndex = 0;
    std::string m_selectedAsset;
    std::string m_metaAsset;
    std::string m_assetDir = "Data";  // current folder (relative to project root)
    bool m_assetGridView = true;
    float m_assetThumbSize = 84.0f;
    std::string m_assetRenameFrom;
    char m_assetRenameBuf[128] = { 0 };
    char m_assetNewNameBuf[96] = { 0 };
    int m_assetNewKind = -1;  // 0 folder, 1 script, 2 screen
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
    std::vector<std::string> m_recentProjects;
    std::string m_assetPendingDelete;
    int m_breakpointLine = 0;
    int m_vmLastPc = -1;
    std::string m_dbBaseline;
    bool m_dbEditPending = false;
    std::string m_pdsTextBuf;
    bool m_pdsTextEditing = false;
    bool m_nodeEditorFocused = false;  // routes Ctrl+Z/Y to the graph's undo

    // Localization table: source line -> translation for Data/Lang/<lang>.json.
    char m_locLang[16] = "en";
    std::vector<std::pair<std::string, std::string>> m_locEntries;
    bool m_locDirty = false;
    char m_locFilter[128] = { 0 };
    std::vector<std::string> m_scriptNameCache;
    std::uint64_t m_scriptNameRevision = ~0ull;
};

}
