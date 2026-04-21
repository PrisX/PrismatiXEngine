#define IMGUI_DEFINE_MATH_OPERATORS

#include "EditorApp.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "Utils/Logger.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

namespace {
constexpr std::string_view kProjectManifestName = "project.prismatix-project";

std::string SanitizeProjectFolderName(std::string value) {
    if (value.empty()) {
        return "PrismatiXProject";
    }
    for (char& ch : value) {
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }
    return value;
}

bool DirectoryHasEntries(const fs::path& path) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        return false;
    }
    return fs::directory_iterator(path) != fs::directory_iterator();
}

bool IsUnder(const fs::path& candidate, const fs::path& root) {
    if (root.empty()) {
        return false;
    }
    std::error_code error;
    const fs::path relative = fs::relative(candidate, root, error);
    return !error && !relative.empty() && relative.generic_string().rfind("..", 0) != 0;
}

bool IsSameOrUnder(const fs::path& candidate, const fs::path& root) {
    return !root.empty() && (candidate.lexically_normal() == root.lexically_normal() || IsUnder(candidate, root));
}
}  // namespace

EditorApp::EditorApp()
    : m_entrypointEditor(BlueprintFlavor::Entrypoint, [this](const std::string& message) { Log(message); }),
      m_sceneEditor(BlueprintFlavor::SceneScript, [this](const std::string& message) { Log(message); }),
      m_uiDesigner([this](const std::string& message) { Log(message); }),
      m_gamePreview([this](const std::string& message) { Log(message); }) {}

EditorApp::~EditorApp() {
    Shutdown();
}

bool EditorApp::Initialize(int argc, char* argv[]) {
    m_workspaceRoot = DetectWorkspaceRoot(argc, argv);
    m_projectLocationInput = DefaultProjectsRoot().string();
    std::filesystem::create_directories("logs");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        PX_LOG_CRITICAL("SDL_Init failed for editor: {}", SDL_GetError());
        return false;
    }

    const int imageFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    if ((IMG_Init(imageFlags) & imageFlags) != imageFlags) {
        PX_LOG_CRITICAL("IMG_Init failed for editor: {}", IMG_GetError());
        return false;
    }

    m_window = SDL_CreateWindow(
        "PrismatiX Editor",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1720,
        1040,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!m_window) {
        PX_LOG_CRITICAL("Failed to create editor window: {}", SDL_GetError());
        return false;
    }

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!m_renderer) {
        PX_LOG_CRITICAL("Failed to create editor renderer: {}", SDL_GetError());
        return false;
    }

    UpdateWindowTitle();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    m_imguiIniPath = (m_workspaceRoot / "logs" / "PrismatiXEditorLayout.ini").string();
    io.IniFilename = m_imguiIniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    SetupStyle();
    SetupFonts();

    ImGui_ImplSDL2_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer2_Init(m_renderer);

    const fs::path blueprintHeaderPath = EditorVendorPath("imgui-node-editor/assets/BlueprintBackground.png");
    if (SDL_Texture* headerTexture = GetTexture(blueprintHeaderPath)) {
        int headerWidth = 0;
        int headerHeight = 0;
        SDL_QueryTexture(headerTexture, nullptr, nullptr, &headerWidth, &headerHeight);
        const ImTextureID headerTextureId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(headerTexture));
        m_entrypointEditor.SetHeaderTexture(headerTextureId, headerWidth, headerHeight);
        m_sceneEditor.SetHeaderTexture(headerTextureId, headerWidth, headerHeight);
    } else {
        Log("Blueprint header texture could not be loaded: " + blueprintHeaderPath.string());
    }

    m_entrypointEditor.SetSelectedResourceCallback([this]() { return CurrentSelectedResource(); });
    m_sceneEditor.SetSelectedResourceCallback([this]() { return CurrentSelectedResource(); });
    m_uiDesigner.SetSelectedResourceCallback([this]() { return CurrentSelectedResource(); });
    m_sceneEditor.SetProjectRoot({});
    m_uiDesigner.SetProjectRoot({});
    m_gamePreview.SetWorkspaceRoot(m_workspaceRoot);

    Log("Workspace root: " + m_workspaceRoot.string());
    Log("Editor initialized. Open or create a separate game project.");
    m_running = true;
    return true;
}

int EditorApp::Run() {
    Uint64 lastCounter = SDL_GetPerformanceCounter();
    while (m_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                m_running = false;
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(m_window)) {
                m_running = false;
            }
        }

        const Uint64 currentCounter = SDL_GetPerformanceCounter();
        const float deltaSeconds = static_cast<float>(currentCounter - lastCounter) / static_cast<float>(SDL_GetPerformanceFrequency());
        lastCounter = currentCounter;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        RenderFrame(deltaSeconds);

        ImGui::Render();
        SDL_SetRenderDrawColor(m_renderer, 6, 6, 8, 255);
        SDL_RenderClear(m_renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), m_renderer);
        SDL_RenderPresent(m_renderer);
    }

    return 0;
}

void EditorApp::Shutdown() {
    m_gamePreview.Shutdown();
    ClearTextures();

    if (ImGui::GetCurrentContext()) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }

    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }

    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    IMG_Quit();
    SDL_Quit();
}

void EditorApp::SetupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.97f, 0.97f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.52f, 0.56f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.03f, 0.04f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.05f, 0.06f, 0.98f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.13f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.03f, 0.03f, 0.04f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.03f, 0.03f, 0.04f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.22f, 0.22f, 0.28f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.34f, 0.34f, 0.40f, 0.80f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.50f, 0.50f, 0.58f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.85f, 0.85f, 0.90f, 0.22f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.02f, 0.02f, 0.03f, 1.00f);
}

void EditorApp::SetupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;

    const auto loadFont = [&](const std::vector<fs::path>& candidates, float size) -> ImFont* {
        for (const fs::path& candidate : candidates) {
            if (fs::exists(candidate)) {
                if (ImFont* font = io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), size, &config, io.Fonts->GetGlyphRangesChineseFull())) {
                    return font;
                }
            }
        }
        return nullptr;
    };

    m_bodyFont = loadFont(
        {
            m_workspaceRoot / "Data" / "Font" / "NotoSansTC-Bold.ttf",
            fs::path("C:/Windows/Fonts/msyh.ttc"),
            fs::path("C:/Windows/Fonts/segoeui.ttf"),
            EditorVendorPath("imgui-node-editor/assets/Play-Regular.ttf"),
        },
        18.0f);

    m_headingFont = loadFont(
        {
            m_workspaceRoot / "Data" / "Font" / "NotoSansTC-Bold.ttf",
            fs::path("C:/Windows/Fonts/msyhbd.ttc"),
            fs::path("C:/Windows/Fonts/seguisb.ttf"),
            EditorVendorPath("imgui-node-editor/assets/Cuprum-Bold.ttf"),
        },
        22.0f);

    if (!m_bodyFont) {
        m_bodyFont = io.Fonts->AddFontDefault();
    }
    if (!m_headingFont) {
        m_headingFont = m_bodyFont;
    }
    io.FontDefault = m_bodyFont;
}

void EditorApp::RenderWelcomeScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("PrismatiXWelcomeScreen", nullptr, flags);
    ImGui::PopStyleVar(2);

    const ImVec2 cardSize(std::min(900.0f, viewport->WorkSize.x - 96.0f), std::min(580.0f, viewport->WorkSize.y - 72.0f));
    ImGui::SetCursorPos(ImVec2((viewport->WorkSize.x - cardSize.x) * 0.5f, (viewport->WorkSize.y - cardSize.y) * 0.5f));
    ImGui::BeginChild("welcome-card", cardSize, ImGuiChildFlags_Borders);

    ImGui::PushFont(m_headingFont ? m_headingFont : ImGui::GetFont());
    ImGui::TextUnformatted("Open a PrismatiX project");
    ImGui::PopFont();
    ImGui::TextDisabled("Game projects should live outside the engine source repo. This workspace is only the editor/runtime codebase.");
    ImGui::Spacing();

    ImGui::InputText("Project Name", &m_projectNameInput);
    ImGui::InputText("Location", &m_projectLocationInput);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const fs::path selectedFolder = BrowseForFolder(DefaultProjectsRoot(), "Choose Project Parent Folder");
        if (!selectedFolder.empty()) {
            m_projectLocationInput = selectedFolder.string();
        }
    }

    if (ImGui::Button("Create Empty Project", ImVec2(180.0f, 42.0f))) {
        CreateProjectFromWelcome(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Create Sample Project", ImVec2(190.0f, 42.0f))) {
        CreateProjectFromWelcome(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Existing Folder", ImVec2(200.0f, 42.0f))) {
        const fs::path selectedFolder = BrowseForFolder(DefaultProjectsRoot(), "Open PrismatiX Project Folder");
        if (!selectedFolder.empty()) {
            OpenProjectFolder(selectedFolder);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Expected Layout");
    ImGui::BulletText("A separate project folder outside the PrismatiX source repo");
    ImGui::BulletText("project.prismatix-project");
    ImGui::BulletText("Data/Image, Data/Audio, Data/Script");
    ImGui::BulletText("Data/Script stores chapter1.pds style scene scripts");
    ImGui::BulletText("Data/Scripts/scenes and Data/Scripts/components store UI/runtime Lua");
    ImGui::BulletText("Export output will be written to Export/");
    ImGui::BulletText("Optional local Engine/ override; otherwise shared runtime assets come from the editor workspace");
    ImGui::BulletText("Create Sample Project copies the current workspace Data/ and Engine/ into a new editable external project");

    ImGui::EndChild();
    ImGui::End();
}

bool EditorApp::CreateProjectFromWelcome(bool seedSampleContent) {
    if (m_projectNameInput.empty() || m_projectLocationInput.empty()) {
        Log("Project name and location are required.");
        return false;
    }

    const std::string folderName = SanitizeProjectFolderName(m_projectNameInput);
    const fs::path rootPath = fs::path(m_projectLocationInput) / folderName;
    const fs::path absoluteRootPath = fs::absolute(rootPath);
    if (IsSameOrUnder(absoluteRootPath, m_workspaceRoot)) {
        Log("Project folders must be outside the PrismatiX engine workspace. Choose another folder.");
        return false;
    }
    if (fs::exists(rootPath) && DirectoryHasEntries(rootPath)) {
        Log("Target project folder already exists and is not empty. Choose another name or location.");
        return false;
    }

    m_entrypointEditor.ResetToDefaults();
    m_sceneEditor.ResetToDefaults();
    m_uiDesigner.ResetToDefaults();

    if (seedSampleContent && !SeedProjectWithWorkspaceSample(absoluteRootPath)) {
        return false;
    }

    return OpenProjectFolder(absoluteRootPath, m_projectNameInput);
}

bool EditorApp::SeedProjectWithWorkspaceSample(const fs::path& rootPath) {
    if (!EnsureProjectScaffold(rootPath, m_projectNameInput.empty() ? rootPath.filename().string() : m_projectNameInput, true)) {
        return false;
    }

    const fs::path workspaceDataRoot = m_workspaceRoot / "Data";
    const fs::path workspaceEngineRoot = m_workspaceRoot / "Engine";
    if (!fs::exists(workspaceDataRoot) || !fs::exists(workspaceEngineRoot)) {
        Log("Workspace sample Data/ or Engine/ folder was not found.");
        return false;
    }

    CopyDirectoryContents(workspaceDataRoot, rootPath / "Data");
    CopyDirectoryContents(workspaceEngineRoot, rootPath / "Engine");

    const fs::path sharedScriptsRoot = WorkspaceScriptsRoot();
    if (!sharedScriptsRoot.empty()) {
        CopyDirectoryContents(sharedScriptsRoot, rootPath / "Data" / "Scripts");
    }

    std::error_code error;
    fs::remove(rootPath / "Data" / "Scripts" / "components" / "editor_ui_components.lua", error);
    error.clear();
    fs::remove(rootPath / "Data" / "Scripts" / "scenes" / "editor_ui_scene.lua", error);

    Log("Sample project seeded from workspace Data/ and Engine/.");
    return true;
}

bool EditorApp::OpenProjectFolder(const fs::path& rootPath, const std::string& preferredName) {
    if (rootPath.empty()) {
        return false;
    }

    const fs::path projectRoot = fs::absolute(rootPath);
    if (IsSameOrUnder(projectRoot, m_workspaceRoot)) {
        Log("Project folders must be outside the PrismatiX engine workspace. Choose another folder.");
        return false;
    }

    std::string projectName = preferredName.empty() ? projectRoot.filename().string() : preferredName;
    if (projectName.empty()) {
        projectName = "PrismatiXProject";
    }

    if (!EnsureProjectScaffold(projectRoot, projectName, !fs::exists(projectRoot / "Data"))) {
        return false;
    }

    m_projectRoot = projectRoot;
    m_projectName = projectName;
    m_projectLocationInput = projectRoot.parent_path().string();
    m_projectNameInput = projectName;
    m_selectedAsset.clear();
    m_resourceFilter.clear();
    m_layoutBuilt = false;
    m_activeTab = WorkspaceTab::UIDesign;
    m_lastDocumentTab = WorkspaceTab::UIDesign;
    m_sceneEditor.SetProjectRoot(m_projectRoot);
    m_uiDesigner.SetProjectRoot(m_projectRoot);
    m_gamePreview.SetProjectRoot(m_projectRoot);
    m_gamePreview.SetPreviewDataRoot(PreviewOverrideRoot());
    m_lastUISyncRevision = -1;
    SyncUIDesignerExports();
    m_previewScenePath = m_uiDesigner.GeneratedSceneScriptPath();
    m_gamePreview.SetScenePath(m_previewScenePath);
    UpdateWindowTitle();
    Log("Project opened: " + m_projectRoot.string());
    return true;
}

bool EditorApp::EnsureProjectScaffold(const fs::path& rootPath, const std::string& projectName, bool createFreshManifest) {
    try {
        fs::create_directories(rootPath / "Data" / "Image");
        fs::create_directories(rootPath / "Data" / "Audio");
        fs::create_directories(rootPath / "Data" / "Font");
        fs::create_directories(rootPath / "Data" / "Script");
        fs::create_directories(rootPath / "Data" / "Scripts" / "scenes");
        fs::create_directories(rootPath / "Data" / "Scripts" / "components");
        fs::create_directories(rootPath / "Data" / "Scripts" / "generated");
        fs::create_directories(rootPath / "Save");
        fs::create_directories(rootPath / "Export");

        const fs::path manifestPath = rootPath / kProjectManifestName;
        if (createFreshManifest || !fs::exists(manifestPath)) {
            std::ofstream manifest(manifestPath, std::ios::binary);
            manifest << "name=" << projectName << '\n';
            manifest << "version=1\n";
        }
    } catch (const std::exception& exception) {
        Log(std::string("Failed to prepare project folder: ") + exception.what());
        return false;
    }
    return true;
}

void EditorApp::CloseProject() {
    if (!m_projectRoot.empty()) {
        Log("Closed project: " + m_projectRoot.string());
    }
    m_projectRoot.clear();
    m_projectName.clear();
    m_selectedAsset.clear();
    m_resourceFilter.clear();
    m_layoutBuilt = false;
    m_activeTab = WorkspaceTab::UIDesign;
    m_lastDocumentTab = WorkspaceTab::UIDesign;
    m_lastUISyncRevision = -1;
    m_sceneEditor.SetProjectRoot({});
    m_uiDesigner.SetProjectRoot({});
    m_gamePreview.SetProjectRoot({});
    m_gamePreview.SetPreviewDataRoot({});
    m_gamePreview.Shutdown();
    UpdateWindowTitle();
}

void EditorApp::UpdateWindowTitle() const {
    if (!m_window) {
        return;
    }
    std::string title = "PrismatiX Editor";
    if (!m_projectName.empty()) {
        title += " - " + m_projectName;
    }
    SDL_SetWindowTitle(m_window, title.c_str());
}

void EditorApp::RenderFrame(float deltaSeconds) {
    if (!HasProjectLoaded()) {
        RenderWelcomeScreen();
        return;
    }

    RenderDockspaceHost();
    EnsureDockLayout();
    RenderResources();
    RenderWorkspace();
    RenderInspector();
    RenderOutput();
}

void EditorApp::RenderDockspaceHost() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("PrismatiXEditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    m_dockspaceId = ImGui::GetID("PrismatiXEditorDockspace");
    ImGui::DockSpace(m_dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project Folder...")) {
                const fs::path selectedFolder = BrowseForFolder(m_projectRoot.empty() ? DefaultProjectsRoot() : m_projectRoot.parent_path(), "Open PrismatiX Project Folder");
                if (!selectedFolder.empty()) {
                    OpenProjectFolder(selectedFolder);
                }
            }
            if (HasProjectLoaded() && ImGui::MenuItem("Close Project")) {
                CloseProject();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export Active Lua")) {
                ExportActive();
            }
            if (ImGui::MenuItem("Export All Lua")) {
                ExportAll();
            }
            if (ImGui::MenuItem("Export Folder Build")) {
                ExportGameFolder();
            }
            if (ImGui::MenuItem("Export PDX Build")) {
                ExportGamePdx();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Layout")) {
                m_layoutBuilt = false;
            }
            if (ImGui::MenuItem("Frame Active Graph")) {
                const WorkspaceTab documentTab = CurrentDocumentTab();
                if (documentTab == WorkspaceTab::Entrypoint) {
                    m_entrypointEditor.NavigateToContent();
                } else if (documentTab == WorkspaceTab::SceneScript) {
                    m_sceneEditor.NavigateToContent();
                }
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    ImGui::End();
}

void EditorApp::EnsureDockLayout() {
    if (m_layoutBuilt || m_dockspaceId == 0) {
        return;
    }

    ImGui::DockBuilderRemoveNode(m_dockspaceId);
    ImGui::DockBuilderAddNode(m_dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(m_dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID centerId = m_dockspaceId;
    ImGuiID leftId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.22f, nullptr, &centerId);
    ImGuiID rightId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.27f, nullptr, &centerId);
    ImGuiID bottomId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Down, 0.28f, nullptr, &centerId);

    ImGui::DockBuilderDockWindow("Workspace", centerId);
    ImGui::DockBuilderDockWindow("Resources", leftId);
    ImGui::DockBuilderDockWindow("Inspector", rightId);
    ImGui::DockBuilderDockWindow("Output", bottomId);
    ImGui::DockBuilderFinish(m_dockspaceId);

    m_layoutBuilt = true;
}

fs::path EditorApp::DetectWorkspaceRoot(int argc, char* argv[]) const {
    auto isWorkspaceRoot = [](const fs::path& candidate) {
        return fs::exists(candidate / "PrismatiXEngine" / "CMakeLists.txt") && fs::exists(candidate / "ThirdParty" / "imgui");
    };

    std::vector<fs::path> seeds;
    seeds.push_back(fs::current_path());
    if (argc > 0 && argv && argv[0]) {
        seeds.push_back(fs::absolute(fs::path(argv[0])).parent_path());
    }

    for (const fs::path& seed : seeds) {
        fs::path current = seed;
        while (!current.empty()) {
            if (isWorkspaceRoot(current)) {
                return current;
            }
            if (current == current.root_path()) {
                break;
            }
            current = current.parent_path();
        }
    }

    return fs::current_path();
}

fs::path EditorApp::EditorVendorPath(std::string_view relativePath) const {
    return m_workspaceRoot / "ThirdParty" / fs::path(relativePath);
}

fs::path EditorApp::DefaultProjectsRoot() const {
    if (const char* userProfile = std::getenv("USERPROFILE")) {
        return fs::path(userProfile) / "Documents" / "PrismatiXProjects";
    }
    return m_workspaceRoot / "Projects";
}

fs::path EditorApp::BrowseForFolder(const fs::path& initialPath, std::string_view title) const {
#ifdef _WIN32
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(initResult);

    fs::path selectedPath;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog))) && dialog) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

        const std::wstring wideTitle(title.begin(), title.end());
        dialog->SetTitle(wideTitle.c_str());

        if (!initialPath.empty() && fs::exists(initialPath)) {
            IShellItem* initialFolder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.wstring().c_str(), nullptr, IID_PPV_ARGS(&initialFolder))) && initialFolder) {
                dialog->SetFolder(initialFolder);
                initialFolder->Release();
            }
        }

        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR pathBuffer = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathBuffer)) && pathBuffer) {
                    selectedPath = fs::path(pathBuffer);
                    CoTaskMemFree(pathBuffer);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return selectedPath;
#else
    (void)initialPath;
    (void)title;
    return {};
#endif
}

bool EditorApp::HasProjectLoaded() const {
    return !m_projectRoot.empty();
}

bool EditorApp::UsesRepositoryLayout() const {
    return HasProjectLoaded();
}

fs::path EditorApp::RuntimeDataRoot() const {
    return m_projectRoot / "Data";
}

fs::path EditorApp::RuntimeScriptsRoot() const {
    return RuntimeDataRoot() / "Scripts";
}

fs::path EditorApp::RuntimeSceneRoot() const {
    return RuntimeScriptsRoot() / "scenes";
}

fs::path EditorApp::RuntimeComponentRoot() const {
    return RuntimeScriptsRoot() / "components";
}

fs::path EditorApp::RuntimeExportRoot() const {
    return m_projectRoot / "Export";
}

fs::path EditorApp::RuntimeEngineRoot() const {
    if (HasProjectLoaded() && fs::exists(m_projectRoot / "Engine")) {
        return m_projectRoot / "Engine";
    }
    return m_workspaceRoot / "Engine";
}

fs::path EditorApp::PreviewOverrideRoot() const {
    if (!HasProjectLoaded()) {
        return {};
    }
    return m_projectRoot / "Save" / "__editor_preview_data";
}

fs::path EditorApp::WorkspaceScriptsRoot() const {
    for (const fs::path& candidate : {m_workspaceRoot / "PrismatiXEngine" / "Scripts", m_workspaceRoot / "Scripts"}) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::vector<fs::path> EditorApp::ExplorerRoots() const {
    std::vector<fs::path> roots;
    if (!HasProjectLoaded()) {
        return roots;
    }
    for (const fs::path& candidate : {RuntimeDataRoot(), RuntimeEngineRoot()}) {
        if (fs::exists(candidate)) {
            roots.push_back(candidate);
        }
    }
    return roots;
}

std::string EditorApp::CurrentSelectedResource() const {
    if (m_selectedAsset.empty()) {
        return {};
    }
    return ToRuntimePath(m_selectedAsset);
}

std::string EditorApp::ToRuntimePath(const fs::path& path) const {
    std::error_code error;
    fs::path relative;
    if (IsUnder(path, RuntimeDataRoot())) {
        relative = fs::relative(path, RuntimeDataRoot(), error);
    } else if (IsUnder(path, RuntimeEngineRoot())) {
        relative = fs::relative(path, RuntimeEngineRoot(), error);
    } else {
        relative = fs::relative(path, m_projectRoot, error);
    }
    if (error) {
        relative = path.filename();
    }

    std::string runtimePath = relative.generic_string();
    return runtimePath;
}

EditorApp::WorkspaceTab EditorApp::CurrentDocumentTab() const {
    return (m_activeTab == WorkspaceTab::Export || m_activeTab == WorkspaceTab::Preview) ? m_lastDocumentTab : m_activeTab;
}

std::string EditorApp::WorkspaceTabLabel(WorkspaceTab tab) const {
    switch (tab) {
        case WorkspaceTab::Entrypoint:
            return "Entrypoint";
        case WorkspaceTab::SceneScript:
            return "Scene Graph";
        case WorkspaceTab::UIDesign:
            return "UI Editor";
        case WorkspaceTab::Preview:
            return "Game Preview";
        case WorkspaceTab::Export:
            return "Export Manager";
    }
    return "Workspace";
}

fs::path EditorApp::ExportPathFor(WorkspaceTab tab) const {
    const fs::path outputDir = RuntimeScriptsRoot() / "generated";
    switch (tab) {
        case WorkspaceTab::Entrypoint:
            return RuntimeScriptsRoot() / "entrypoint.lua";
        case WorkspaceTab::SceneScript:
            return m_sceneEditor.CurrentDocumentPath().empty() ? (RuntimeDataRoot() / "Script" / "chapter1.pds") : m_sceneEditor.CurrentDocumentPath();
        case WorkspaceTab::UIDesign: {
            const std::string runtimePath = m_uiDesigner.CurrentDocumentRuntimePath();
            return runtimePath.empty() ? (outputDir / "ui_editor.lua") : (RuntimeDataRoot() / fs::path(runtimePath));
        }
        case WorkspaceTab::Preview:
        case WorkspaceTab::Export:
            break;
    }
    return outputDir;
}

std::string EditorApp::PreviewLabelFor(WorkspaceTab tab) const {
    switch (tab) {
        case WorkspaceTab::SceneScript:
            return "PDS";
        case WorkspaceTab::Entrypoint:
        case WorkspaceTab::UIDesign:
        case WorkspaceTab::Preview:
        case WorkspaceTab::Export:
            return "Lua";
    }
    return "Preview";
}

std::string EditorApp::GenerateDocumentFor(WorkspaceTab tab) const {
    switch (tab) {
        case WorkspaceTab::Entrypoint:
            return m_entrypointEditor.GenerateLua();
        case WorkspaceTab::SceneScript:
            return m_sceneEditor.GenerateLua();
        case WorkspaceTab::UIDesign:
            return m_uiDesigner.GenerateLua();
        case WorkspaceTab::Preview:
        case WorkspaceTab::Export:
            break;
    }
    return {};
}

std::vector<EditorApp::ExportArtifact> EditorApp::BuildExportArtifacts() const {
    std::vector<ExportArtifact> artifacts;

    ExportArtifact entrypoint;
    entrypoint.tab = WorkspaceTab::Entrypoint;
    entrypoint.label = "Entrypoint";
    entrypoint.previewLabel = "Lua";
    entrypoint.path = ExportPathFor(WorkspaceTab::Entrypoint);
    entrypoint.content = m_entrypointEditor.GenerateLua();
    artifacts.push_back(std::move(entrypoint));

    for (const BlueprintEditor::ExportDocument& document : m_sceneEditor.BuildExportDocuments()) {
        ExportArtifact artifact;
        artifact.tab = WorkspaceTab::SceneScript;
        artifact.label = document.label;
        artifact.previewLabel = "PDS";
        artifact.path = RuntimeDataRoot() / document.relativePath;
        artifact.content = document.content;
        artifacts.push_back(std::move(artifact));
    }

    for (const UIDesigner::GeneratedDocument& document : m_uiDesigner.BuildGeneratedDocuments()) {
        ExportArtifact artifact;
        artifact.tab = WorkspaceTab::UIDesign;
        artifact.label = document.label;
        artifact.previewLabel = "Lua";
        artifact.path = m_projectRoot / "Data" / document.relativePath;
        artifact.content = document.content;
        artifacts.push_back(std::move(artifact));
    }

    return artifacts;
}

void EditorApp::SyncUIDesignerExports() {
    if (!HasProjectLoaded()) {
        return;
    }
    const fs::path previewRoot = PreviewOverrideRoot();
    m_gamePreview.SetPreviewDataRoot(previewRoot);
    if (m_lastUISyncRevision == m_uiDesigner.Revision()) {
        return;
    }

    m_lastUISyncRevision = m_uiDesigner.Revision();
    std::error_code error;
    fs::remove_all(previewRoot, error);
    fs::create_directories(previewRoot);

    for (const UIDesigner::GeneratedDocument& document : m_uiDesigner.BuildGeneratedDocuments()) {
        const fs::path outputPath = previewRoot / document.relativePath;
        fs::create_directories(outputPath.parent_path());
        std::ofstream out(outputPath, std::ios::binary);
        out << document.content;
    }

    m_gamePreview.RequestReload();
}

std::vector<std::string> EditorApp::AvailableSceneScripts() const {
    std::vector<std::string> scenes;
    std::vector<fs::path> sceneRoots;
    const fs::path sharedScriptsRoot = WorkspaceScriptsRoot();
    if (!sharedScriptsRoot.empty()) {
        sceneRoots.push_back(sharedScriptsRoot / "scenes");
    }
    sceneRoots.push_back(RuntimeSceneRoot());

    for (const fs::path& sceneRoot : sceneRoots) {
        if (!fs::exists(sceneRoot)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(sceneRoot)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lua") {
                const std::string scenePath = (fs::path("Scripts/scenes") / entry.path().filename()).generic_string();
                if (std::find(scenes.begin(), scenes.end(), scenePath) == scenes.end()) {
                    scenes.push_back(scenePath);
                }
            }
        }
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

SDL_Texture* EditorApp::GetTexture(const fs::path& path) {
    if (!IsImageAsset(path) || !m_renderer) {
        return nullptr;
    }
    const std::string key = path.string();
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) {
        return it->second;
    }
    SDL_Texture* texture = IMG_LoadTexture(m_renderer, key.c_str());
    if (texture) {
        m_textureCache.emplace(key, texture);
    }
    return texture;
}

bool EditorApp::IsImageAsset(const fs::path& path) const {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".webp";
}

void EditorApp::ClearTextures() {
    for (auto& [key, texture] : m_textureCache) {
        (void)key;
        if (texture) {
            SDL_DestroyTexture(texture);
        }
    }
    m_textureCache.clear();
}

void EditorApp::CopyDirectoryContents(const fs::path& sourceDir, const fs::path& destinationDir) {
    if (!fs::exists(sourceDir)) {
        return;
    }
    for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
        const fs::path relative = fs::relative(entry.path(), sourceDir);
        const fs::path target = destinationDir / relative;
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
}

void EditorApp::CopyRuntimeBinaryBundle(const fs::path& runtimeExe, const fs::path& destinationDir) {
    if (runtimeExe.empty() || !fs::exists(runtimeExe)) {
        return;
    }

    fs::create_directories(destinationDir);
    fs::copy_file(runtimeExe, destinationDir / runtimeExe.filename(), fs::copy_options::overwrite_existing);

    const fs::path runtimeDir = runtimeExe.parent_path();
    for (const auto& entry : fs::directory_iterator(runtimeDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const fs::path path = entry.path();
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".dll") {
            continue;
        }

        fs::copy_file(path, destinationDir / path.filename(), fs::copy_options::overwrite_existing);
    }
}

void EditorApp::PrepareExportDataBundle(const fs::path& destinationDir) {
    std::error_code error;
    fs::remove_all(destinationDir, error);
    fs::create_directories(destinationDir);

    const fs::path sharedScriptsRoot = WorkspaceScriptsRoot();
    if (!sharedScriptsRoot.empty()) {
        CopyDirectoryContents(sharedScriptsRoot, destinationDir / "Scripts");
    } else {
        Log("Shared runtime Scripts folder was not found. Exported build may be missing engine Lua modules.");
    }

    CopyDirectoryContents(RuntimeDataRoot(), destinationDir);

    const fs::path entrypointPath = destinationDir / "Scripts" / "entrypoint.lua";
    fs::create_directories(entrypointPath.parent_path());
    {
        std::ofstream out(entrypointPath, std::ios::binary);
        out << m_entrypointEditor.GenerateLua();
    }

    for (const BlueprintEditor::ExportDocument& document : m_sceneEditor.BuildExportDocuments()) {
        const fs::path outputPath = destinationDir / document.relativePath;
        fs::create_directories(outputPath.parent_path());
        std::ofstream out(outputPath, std::ios::binary);
        out << document.content;
    }

    for (const UIDesigner::GeneratedDocument& document : m_uiDesigner.BuildGeneratedDocuments()) {
        const fs::path outputPath = destinationDir / document.relativePath;
        fs::create_directories(outputPath.parent_path());
        std::ofstream out(outputPath, std::ios::binary);
        out << document.content;
    }
}

bool EditorApp::CreatePdxArchive(const fs::path& sourceDir, const fs::path& archivePath) {
    if (!fs::exists(sourceDir)) {
        return false;
    }

    struct PdxFile {
        fs::path fullPath;
        std::string archiveName;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
    };

    std::vector<PdxFile> files;
    for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        PdxFile file;
        file.fullPath = entry.path();
        file.archiveName = fs::relative(entry.path(), sourceDir).generic_string();
        file.size = static_cast<std::uint64_t>(fs::file_size(entry.path()));
        files.push_back(std::move(file));
    }

    std::sort(files.begin(), files.end(), [](const PdxFile& left, const PdxFile& right) {
        return left.archiveName < right.archiveName;
    });

    std::uint64_t tableSize = 4 + sizeof(std::uint32_t);
    for (const PdxFile& file : files) {
        tableSize += sizeof(std::uint16_t) + file.archiveName.size() + sizeof(std::uint64_t) + sizeof(std::uint64_t);
    }

    std::uint64_t currentOffset = tableSize;
    for (PdxFile& file : files) {
        file.offset = currentOffset;
        currentOffset += file.size;
    }

    fs::create_directories(archivePath.parent_path());
    std::ofstream out(archivePath, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    out.write("PDX!", 4);
    const std::uint32_t fileCount = static_cast<std::uint32_t>(files.size());
    out.write(reinterpret_cast<const char*>(&fileCount), sizeof(fileCount));
    for (const PdxFile& file : files) {
        const std::uint16_t nameLength = static_cast<std::uint16_t>(file.archiveName.size());
        out.write(reinterpret_cast<const char*>(&nameLength), sizeof(nameLength));
        out.write(file.archiveName.data(), nameLength);
        out.write(reinterpret_cast<const char*>(&file.offset), sizeof(file.offset));
        out.write(reinterpret_cast<const char*>(&file.size), sizeof(file.size));
    }

    std::vector<char> buffer;
    for (const PdxFile& file : files) {
        buffer.resize(static_cast<size_t>(file.size));
        std::ifstream in(file.fullPath, std::ios::binary);
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }

    return true;
}

fs::path EditorApp::FindRuntimeExecutable() const {
    fs::path newest;
    fs::file_time_type newestTime{};
    bool found = false;

    for (const fs::path& buildRoot : {m_workspaceRoot / "out" / "build", m_workspaceRoot / "build"}) {
        if (!fs::exists(buildRoot)) {
            continue;
        }

        for (const auto& entry : fs::recursive_directory_iterator(buildRoot)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().filename() != "PrismatiXEngine.exe") {
                continue;
            }
            const auto writeTime = fs::last_write_time(entry.path());
            if (!found || writeTime > newestTime) {
                newest = entry.path();
                newestTime = writeTime;
                found = true;
            }
        }
    }
    return newest;
}

void EditorApp::ExportGameFolder() {
    if (!HasProjectLoaded()) {
        return;
    }

    SyncUIDesignerExports();
    const fs::path targetRoot = RuntimeExportRoot() / "folder_build";
    const fs::path stagedDataRoot = RuntimeExportRoot() / ".staging" / "folder_data";
    std::error_code error;
    fs::remove_all(targetRoot, error);
    fs::create_directories(targetRoot);
    PrepareExportDataBundle(stagedDataRoot);

    const fs::path runtimeExe = FindRuntimeExecutable();
    if (!runtimeExe.empty()) {
        CopyRuntimeBinaryBundle(runtimeExe, targetRoot);
    } else {
        Log("Runtime executable not found under build outputs. Folder export will only contain data and engine assets.");
    }

    CopyDirectoryContents(stagedDataRoot, targetRoot / "Data");
    CopyDirectoryContents(RuntimeEngineRoot(), targetRoot / "Engine");
    Log("Exported folder build to " + targetRoot.string());
}

void EditorApp::ExportGamePdx() {
    if (!HasProjectLoaded()) {
        return;
    }

    SyncUIDesignerExports();
    const fs::path targetRoot = RuntimeExportRoot() / "pdx_build";
    const fs::path stagedDataRoot = RuntimeExportRoot() / ".staging" / "pdx_data";
    std::error_code error;
    fs::remove_all(targetRoot, error);
    fs::create_directories(targetRoot);
    PrepareExportDataBundle(stagedDataRoot);

    const fs::path runtimeExe = FindRuntimeExecutable();
    if (!runtimeExe.empty()) {
        CopyRuntimeBinaryBundle(runtimeExe, targetRoot);
    } else {
        Log("Runtime executable not found under build outputs. PDX export will only contain archives.");
    }

    if (!CreatePdxArchive(stagedDataRoot, targetRoot / "Data.pdx")) {
        Log("Failed to create Data.pdx");
    }
    if (fs::exists(RuntimeEngineRoot()) && !CreatePdxArchive(RuntimeEngineRoot(), targetRoot / "Engine.pdx")) {
        Log("Failed to create Engine.pdx");
    }
    Log("Exported PDX build to " + targetRoot.string());
}

void EditorApp::Log(const std::string& message) {
    m_logs.push_back(message);
    if (m_logs.size() > 200) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + 50);
    }
    PX_LOG_INFO("{}", message);
}

}  // namespace PrismatiX::Editor
