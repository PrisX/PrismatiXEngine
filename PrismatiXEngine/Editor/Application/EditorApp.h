#pragma once

#include "Editor/Build/BuildService.h"
#include "Editor/Tools/Story/StoryLibrary.h"
#include "Editor/Project/ProjectService.h"
#include "Engine/Platform/Window.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Styles/StyleDefinition.h"

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace px::editor {

enum class EditorWorkspace { UI, Story, Script };

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
    Status EnsureAssetIdentity(const std::filesystem::path& path);
    Status EnsureAssetIdentities(const std::vector<std::filesystem::path>& paths);
    void LoadFonts();
    void BuildDockLayout(unsigned int dockspaceId);
    void SetWorkspace(EditorWorkspace workspace);
    void RenderWorkspaceSwitcher();
    void RenderQuickOpen();
    void RenderStatusBar();
    void RenderDiagnosticToasts();
    void RenderWelcome();
    void RenderMenuBar();
    void RenderInspector();
    void OpenAssetByType(const resource::AssetEntry& entry);
    [[nodiscard]] std::string AssetRuntimePath(const resource::AssetEntry& entry) const;
    [[nodiscard]] static std::string ClassifyAsset(const std::filesystem::path& path);
    Status CreateAssetWithHistory(const std::filesystem::path& absolutePath, int kind);
    void RefreshAfterProjectMutation();
    void RenderConsole();
    void RenderPreview();
    void RenderNarrative();
    void RenderStoryLibrary();
    void RenderBuild();
    bool RenderSplashSettings(ProjectManifest& manifest);
    void RenderAnimation();
    void RenderTheme();
    void RenderScripting();
    void RenderProblems();
    void RenderAssetIdentityResolver();
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
    void LoadRecentProjects();
    void AddRecentProject(const std::filesystem::path& root);
    void RunBuild();
    void RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir);
    void RunDev();
    void RunPackaged();
    void OpenInExplorer(const std::filesystem::path& path);
    void SaveAll();
    void Log(const std::string& message);

    px::Window m_window;
    ProjectService m_project;
    resource::AssetRegistry m_assetRegistry;

    StoryLibrary m_storyLibrary;

    std::optional<animation::AnimationClip> m_timelineClip;
    std::string m_timelinePath;
    float m_timelineCursor = 0.0f;
    bool m_timelinePlaying = false;
    int m_timelineTrack = -1;
    std::optional<resource::TypedDocument> m_themeDocument;
    std::optional<ui::StyleThemeData> m_themeData;
    std::string m_themePath;
    bool m_themeDirty = false;
    int m_themeToken = -1;
    int m_themeStyle = -1;
    int m_themeAxis = -1;
    std::string m_themeTraceControl = "Button";
    int m_themeTraceStyle = -1;
    std::vector<std::string> m_console;
    std::string m_selectedAsset;
    int m_buildProfile = 1;
    int m_splashSelection = 0;
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
    bool m_showAssetIdentity = false;
    bool m_showBuildWindow = false;
    bool m_showLocalizationWindow = false;
    bool m_bottomDrawerExpanded = true;
    char m_paletteFilter[128] = { 0 };
    // Project-wide validation results retain their source location so the
    // Problems panel can navigate to the exact Scenario node/property.
    std::vector<diag::Diagnostic> m_problems;

    enum class Screen { Welcome, Workspace };
    Screen m_screen = Screen::Welcome;
    bool m_buildLayout = false;
    char m_openPath[512] = { 0 };
    char m_newName[128] = { 0 };
    char m_newPath[512] = { 0 };
    std::string m_basePath;
    std::string m_iniPath;
    std::vector<std::string> m_recentProjects;
    int m_breakpointLine = 0;
    EditorWorkspace m_workspace = EditorWorkspace::UI;
    std::array<bool, 3> m_workspaceLayoutDirty{true, true, true};
    DiagnosticToastQueue m_toasts;
    char m_quickOpenFilter[160] = { 0 };
    bool m_quickOpenOpen = false;

    struct LocalizationEntry { std::string id; std::string source; std::string translation; };
    char m_locLang[16] = "en";
    std::vector<LocalizationEntry> m_locEntries;
    bool m_locDirty = false;
    char m_locFilter[128] = { 0 };
};

}
