#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "BlueprintEditor.h"
#include "GamePreviewHost.h"
#include "UIDesigner.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;
struct ImFont;
using ImGuiID = unsigned int;

namespace PrismatiX::Editor {

class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    bool Initialize(int argc, char* argv[]);
    int Run();

private:
    enum class WorkspaceTab {
        Entrypoint,
        SceneScript,
        UIDesign,
        Preview,
        Export,
    };

    struct ExportArtifact {
        WorkspaceTab tab = WorkspaceTab::Entrypoint;
        std::string label;
        std::string previewLabel;
        std::filesystem::path path;
        std::string content;
    };

    void Shutdown();
    void SetupStyle();
    void SetupFonts();
    void RenderFrame(float deltaSeconds);
    void RenderWelcomeScreen();
    void EnsureDockLayout();
    void RenderDockspaceHost();
    void RenderWorkspace();
    void RenderResources();
    void RenderInspector();
    void RenderOutput();
    void RenderExportManager();
    void RenderActivityLog();
    void RenderAssetPreview();
    void ClearTextures();
    void Log(const std::string& message);
    void RenderGamePreviewTab();
    [[nodiscard]] UIDesigner::RuntimeCanvasResult RenderRuntimeScene(const std::string& scenePath, int logicalWidth, int logicalHeight, int mouseX, int mouseY, bool leftClick, bool rightClick);
    void SyncUIDesignerExports();
    void ExportGameFolder();
    void ExportGamePdx();
    void CopyRuntimeBinaryBundle(const std::filesystem::path& runtimeExe, const std::filesystem::path& destinationDir);
    void CopyDirectoryContents(const std::filesystem::path& sourceDir, const std::filesystem::path& destinationDir);
    void PrepareExportDataBundle(const std::filesystem::path& destinationDir);
    bool CreatePdxArchive(const std::filesystem::path& sourceDir, const std::filesystem::path& archivePath);

    void ExportActive();
    void ExportAll();
    void ExportDocument(const std::filesystem::path& filePath, const std::string& label, const std::string& content);
    void CloseProject();
    bool CreateProjectFromWelcome(bool seedSampleContent = false);
    bool OpenProjectFolder(const std::filesystem::path& rootPath, const std::string& preferredName = {});
    bool EnsureProjectScaffold(const std::filesystem::path& rootPath, const std::string& projectName, bool createFreshManifest);
    bool SeedProjectWithWorkspaceSample(const std::filesystem::path& rootPath);
    void UpdateWindowTitle() const;

    [[nodiscard]] std::filesystem::path DetectWorkspaceRoot(int argc, char* argv[]) const;
    [[nodiscard]] std::filesystem::path EditorVendorPath(std::string_view relativePath) const;
    [[nodiscard]] std::filesystem::path DefaultProjectsRoot() const;
    [[nodiscard]] std::filesystem::path BrowseForFolder(const std::filesystem::path& initialPath, std::string_view title) const;
    [[nodiscard]] bool HasProjectLoaded() const;
    [[nodiscard]] std::vector<std::filesystem::path> ExplorerRoots() const;
    [[nodiscard]] std::string CurrentSelectedResource() const;
    [[nodiscard]] std::string ToRuntimePath(const std::filesystem::path& path) const;
    [[nodiscard]] WorkspaceTab CurrentDocumentTab() const;
    [[nodiscard]] std::string WorkspaceTabLabel(WorkspaceTab tab) const;
    [[nodiscard]] std::filesystem::path ExportPathFor(WorkspaceTab tab) const;
    [[nodiscard]] std::string PreviewLabelFor(WorkspaceTab tab) const;
    [[nodiscard]] std::string GenerateDocumentFor(WorkspaceTab tab) const;
    [[nodiscard]] std::vector<ExportArtifact> BuildExportArtifacts() const;
    [[nodiscard]] SDL_Texture* GetTexture(const std::filesystem::path& path);
    [[nodiscard]] bool IsImageAsset(const std::filesystem::path& path) const;
    [[nodiscard]] bool UsesRepositoryLayout() const;
    [[nodiscard]] std::filesystem::path RuntimeDataRoot() const;
    [[nodiscard]] std::filesystem::path RuntimeScriptsRoot() const;
    [[nodiscard]] std::filesystem::path RuntimeSceneRoot() const;
    [[nodiscard]] std::filesystem::path RuntimeComponentRoot() const;
    [[nodiscard]] std::filesystem::path RuntimeExportRoot() const;
    [[nodiscard]] std::filesystem::path RuntimeEngineRoot() const;
    [[nodiscard]] std::filesystem::path PreviewOverrideRoot() const;
    [[nodiscard]] std::filesystem::path WorkspaceScriptsRoot() const;
    [[nodiscard]] std::vector<std::string> AvailableSceneScripts() const;
    [[nodiscard]] std::filesystem::path FindRuntimeExecutable() const;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    bool m_running = false;
    bool m_layoutBuilt = false;
    ImGuiID m_dockspaceId = 0;
    WorkspaceTab m_activeTab = WorkspaceTab::Entrypoint;
    WorkspaceTab m_lastDocumentTab = WorkspaceTab::Entrypoint;
    std::filesystem::path m_workspaceRoot;
    std::filesystem::path m_projectRoot;
    std::string m_projectName;
    std::string m_projectNameInput = "MyNovelProject";
    std::string m_projectLocationInput;
    std::string m_imguiIniPath;
    std::filesystem::path m_selectedAsset;
    std::string m_resourceFilter;
    std::vector<std::string> m_logs;
    std::unordered_map<std::string, SDL_Texture*> m_textureCache;

    ImFont* m_bodyFont = nullptr;
    ImFont* m_headingFont = nullptr;

    BlueprintEditor m_entrypointEditor;
    BlueprintEditor m_sceneEditor;
    UIDesigner m_uiDesigner;
    GamePreviewHost m_gamePreview;
    std::string m_previewScenePath = "Scripts/scenes/title_scene.lua";
    int m_lastUISyncRevision = -1;
};

}  // namespace PrismatiX::Editor
