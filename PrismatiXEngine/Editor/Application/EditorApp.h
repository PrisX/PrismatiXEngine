#pragma once

#include "Editor/Build/BuildService.h"
#include "Editor/Assets/AssetDatabase.h"
#include "Editor/Assets/EditorTextures.h"
#include "Editor/Assets/ImportService.h"
#include "Editor/Tools/Flow/FlowMap.h"
#include "Editor/Tools/NodeEditor/NodeGraphEditor.h"
#include "Editor/Preview/RuntimeHost.h"
#include "Editor/Tools/Lua/ScriptWorkspace.h"
#include "Editor/Project/ProjectService.h"
#include "Editor/Tools/UIDesigner/UIDesigner.h"
#include "Editor/Workspace/DocumentRegistry.h"
#include "Editor/Workspace/RecoveryManager.h"
#include "Editor/Workspace/ProjectHistory.h"
#include "Engine/Platform/Window.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Animation/Timeline.h"

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace px::editor {

enum class FileSystemViewMode { Details, Compact, Thumbnails };

struct AssetSelectionModel {
    std::unordered_set<std::string> selected;
    std::string anchor;
    void Clear() { selected.clear(); anchor.clear(); }
    [[nodiscard]] bool Contains(const std::string& path) const { return selected.contains(path); }
};

struct DiagnosticToast {
    diag::Diagnostic diagnostic;
    std::chrono::steady_clock::time_point created;
};

class DiagnosticToastQueue {
public:
    void Push(const diag::Diagnostic& diagnostic);
    void Prune(std::chrono::steady_clock::time_point now);
    void Clear() { m_items.clear(); }
    [[nodiscard]] const std::deque<DiagnosticToast>& Items() const { return m_items; }
private:
    std::deque<DiagnosticToast> m_items;
};

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
    void ConfigureDesigner(UIDesigner& designer);
    Status ActivateUIDocument(const std::filesystem::path& absolutePath,
                              const std::string& runtimePath = {});
    void ApplyTheme();
    void LoadFonts();
    void BuildDockLayout(unsigned int dockspaceId);
    void SetWorkspace(EditorWorkspace workspace);
    void RenderWorkspaceSwitcher();
    void RenderOpenDocuments();
    void RenderStatusBar();
    void RenderDiagnosticToasts();
    void RenderWelcome();
    void RenderMenuBar();
    void RenderHierarchy();
    void RenderInspector();
    void RenderAssets();
    void RenderAssetTree(const std::filesystem::path& dir, const std::filesystem::path& root,
                         std::size_t depth = 0);
    void RenderAssetEntry(const AssetRecord& rec, bool gridMode, float tile);
    void SetAssetDirectory(std::string runtimePath, bool recordHistory = true,
                           bool clearForwardHistory = true);
    void OpenAssetByType(const AssetRecord& rec);
    void MoveAssetTo(const std::string& runtimePath, const std::filesystem::path& targetDir);
    Status MoveAssetWithHistory(const std::string& oldRuntimePath,
                                const std::string& newRuntimePath);
    Status TrashAssetWithHistory(const std::string& runtimePath);
    Status CreateAssetWithHistory(const std::filesystem::path& absolutePath, int kind);
    Status DuplicateAssetWithHistory(const std::string& runtimePath);
    void RefreshAfterProjectMutation();
    void OnAssetRelocated(const std::string& fromRuntimePath,
                          const std::string& toRuntimePath);
    void RenderConsole();
    void RenderPreview();
    void RenderNodeEditor();
    void RenderBuild();
    void RenderAnimation();
    void RenderTheme();
    void RenderFlow();
    void RenderScripting();
    void RenderProblems();
    void RenderRecoveryCenter();
    void RenderAssetIdentityResolver();
    void RenderProjectTrash();
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
    [[nodiscard]] const std::vector<std::string>& ScriptFileNames();
    void CreateScriptFile(const std::string& script);
    void ConnectStoryScenarios(const std::string& fromScenario, const std::string& toScenario);
    void DisconnectStoryScenarios(const std::string& fromScenario, const std::string& toScenario);
    void LoadRecentProjects();
    void AddRecentProject(const std::filesystem::path& root);
    void RunBuild();
    void RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir);
    void RunDev();
    void RunPackaged();
    void OpenInExplorer(const std::filesystem::path& path);
    void ImportAssetFiles(const std::vector<std::filesystem::path>& paths);
    void ImportClipboardAssets();
    void QueueImportReview(const std::vector<std::filesystem::path>& paths);
    void RenderImportReview();
    void SaveAll();
    void CreateFlowChapter(ImVec2 canvasPosition);
    void Log(const std::string& message);

    px::Window m_window;
    ProjectService m_project;
    AssetDatabase m_assets;
    DocumentManager m_docs;
    RecoveryManager m_recovery;
    resource::AssetRegistry m_assetRegistry;
    ImportService m_importService;
    ProjectCommandHistory m_projectHistory;

    std::unique_ptr<RuntimeHost> m_preview;
    std::unique_ptr<EditorTextures> m_textures;
    ImTextureID m_nodeHeaderTex{};
    int m_nodeHeaderW = 0;
    int m_nodeHeaderH = 0;
    UIDesigner m_designer;
    std::string m_designerPath;
    std::unordered_map<std::string, DesignerDocumentSession> m_inactiveDesigners;
    char m_newScreenName[96] = "new_screen";

    // Open Scenario documents, one synchronized workspace per tab.
    std::vector<std::unique_ptr<NodeGraphEditor>> m_scriptDocs;
    int m_activeDoc = -1;
    int m_focusDocRequest = -1;
    std::vector<CustomCommandDef> m_customCommands;
    [[nodiscard]] NodeGraphEditor* ActiveDocPtr();
    NodeGraphEditor* OpenDocTab(const std::string& runtimePath);
    void ConfigureDoc(NodeGraphEditor& doc);
    void TrackDocument(const std::filesystem::path& absolutePath, DocumentType type,
                       bool dirty = false);
    void SyncDocumentStates();
    void SaveEditorSession();
    void RestoreEditorSession();
    void CheckExternalDocuments();
    void RenderExternalDocumentConflict();

    bool m_flowStale = false;
    bool m_previewAnims = false;
    std::optional<animation::AnimationClip> m_timelineClip;
    std::string m_timelinePath;
    float m_timelineCursor = 0.0f;
    bool m_timelinePlaying = false;
    int m_timelineTrack = -1;
    FlowMap m_flow;
    ScriptWorkspace m_scripts;

    std::vector<std::string> m_console;
    char m_assetFilter[128] = { 0 };
    int m_assetTypeIndex = 0;
    int m_assetStatusFilter = 0;
    std::string m_selectedAsset;
    std::string m_metaAsset;
    std::string m_assetDir = "Content";  // current folder (relative to project root)
    enum class AssetSortColumn { Name, Type, Size, Modified };
    FileSystemViewMode m_fileSystemView = FileSystemViewMode::Details;
    AssetSortColumn m_assetSortColumn = AssetSortColumn::Name;
    bool m_assetSortAscending = true;
    bool m_assetsFocused = false;
    float m_assetRowHeight = 28.0f;
    float m_assetThumbSize = 84.0f;
    struct FolderViewSettings {
        FileSystemViewMode mode = FileSystemViewMode::Details;
        AssetSortColumn sort = AssetSortColumn::Name;
        bool ascending = true;
        float rowHeight = 28.0f;
        float thumbnailSize = 84.0f;
    };
    std::unordered_map<std::string, FolderViewSettings> m_folderViewSettings;
    std::vector<std::string> m_assetDirectoryHistory;
    std::vector<std::string> m_assetDirectoryForward;
    std::string m_assetPathInput = "Content";
    bool m_showAssetFolderTree = true;
    AssetSelectionModel m_assetSelectionModel;
    std::optional<ImportPlan> m_importReview;
    std::vector<ImportSource> m_importSources;
    std::string m_importDestinationText = "Content";
    bool m_showImportReview = false;
    bool m_importAutoOrganize = false;
    bool m_importPreserveFolders = true;
    bool m_importPreserveIdentity = false;
    std::string m_reportedImportFailure;
    std::string m_assetRenameFrom;
    char m_assetRenameBuf[128] = { 0 };
    char m_assetNewNameBuf[96] = { 0 };
    int m_assetNewKind = -1;  // 0 folder, 1 script, 2 screen
    int m_previewMode = 0;
    int m_buildProfile = 1;
    bool m_running = false;
    bool m_imguiReady = false;
    bool m_iconFontLoaded = false;

    struct PaletteCommand {
        std::string label;
        std::string shortcut;
        std::function<void()> run;
    };
    std::vector<PaletteCommand> m_commands;
    bool m_paletteOpen = false;
    bool m_paletteFocus = false;
    bool m_showShortcuts = false;
    bool m_showRecoveryCenter = false;
    bool m_showAssetIdentity = false;
    bool m_showBuildWindow = false;
    bool m_showLocalizationWindow = false;
    bool m_showProjectTrash = false;
    bool m_showOpenDocuments = true;
    bool m_bottomDrawerExpanded = true;
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
    std::filesystem::path m_editorSettingsPath;
    std::filesystem::path m_editorSessionPath;
    std::vector<std::string> m_recentProjects;
    std::string m_assetPendingDelete;
    int m_breakpointLine = 0;
    int m_vmLastPc = -1;
    bool m_nodeEditorFocused = false;  // routes Ctrl+Z/Y to the graph's undo
    EditorWorkspace m_workspace = EditorWorkspace::UI;
    std::array<bool, 4> m_workspaceLayoutDirty{true, true, true, true};
    DiagnosticToastQueue m_toasts;
    int m_documentCloseRequest = -1;
    bool m_documentClosePopup = false;
    std::filesystem::path m_uiDocumentCloseRequest;
    bool m_uiDocumentClosePopup = false;
    char m_quickOpenFilter[160] = { 0 };
    bool m_quickOpenOpen = false;
    std::filesystem::path m_externalConflictPath;
    char m_externalSaveAsPath[512] = { 0 };

    struct LocalizationEntry { std::string id; std::string source; std::string translation; };
    char m_locLang[16] = "en";
    std::vector<LocalizationEntry> m_locEntries;
    bool m_locDirty = false;
    char m_locFilter[128] = { 0 };
    std::vector<std::string> m_scriptNameCache;
    std::uint64_t m_scriptNameRevision = ~0ull;
};

}
