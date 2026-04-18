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

#include "Utils/Logger.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

EditorApp::EditorApp()
    : m_entrypointEditor(BlueprintFlavor::Entrypoint, [this](const std::string& message) { Log(message); }),
      m_sceneEditor(BlueprintFlavor::SceneScript, [this](const std::string& message) { Log(message); }),
      m_uiDesigner([this](const std::string& message) { Log(message); }) {}

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

    Log("Workspace root: " + m_workspaceRoot.string());
    Log("Editor initialized with split game/editor executables.");
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
        SDL_SetRenderDrawColor(m_renderer, 9, 12, 20, 255);
        SDL_RenderClear(m_renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), m_renderer);
        SDL_RenderPresent(m_renderer);
    }

    return 0;
}

void EditorApp::Shutdown() {
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
    style.WindowRounding = 14.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 9.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 10.0f;
    style.TabRounding = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.61f, 0.69f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.07f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.09f, 0.14f, 0.96f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.16f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.19f, 0.24f, 0.32f, 0.80f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.13f, 0.20f, 0.96f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.20f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.24f, 0.37f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.08f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.05f, 0.07f, 0.11f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.17f, 0.24f, 0.35f, 0.90f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.33f, 0.47f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.40f, 0.58f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.28f, 0.44f, 0.94f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.38f, 0.58f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.44f, 0.67f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.14f, 0.22f, 0.95f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.35f, 0.54f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.19f, 0.31f, 0.48f, 1.00f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.08f, 0.10f, 0.16f, 0.90f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.13f, 0.18f, 0.28f, 0.95f);
    colors[ImGuiCol_Separator] = ImVec4(0.21f, 0.27f, 0.36f, 0.70f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.25f, 0.50f, 0.78f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.36f, 0.62f, 0.90f, 0.78f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.44f, 0.72f, 1.00f, 0.92f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.35f, 0.64f, 0.92f, 0.30f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.04f, 0.06f, 0.10f, 1.00f);
}

void EditorApp::SetupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;

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
        {fs::path("C:/Windows/Fonts/msyh.ttc"),
         fs::path("C:/Windows/Fonts/segoeui.ttf"),
         EditorVendorPath("imgui-node-editor/assets/Play-Regular.ttf")},
        19.0f);

    m_headingFont = loadFont(
        {fs::path("C:/Windows/Fonts/msyhbd.ttc"),
         fs::path("C:/Windows/Fonts/seguisb.ttf"),
         EditorVendorPath("imgui-node-editor/assets/Cuprum-Bold.ttf")},
        23.0f);

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("PrismatiXWelcomeScreen", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = min + ImGui::GetWindowSize();
    drawList->AddRectFilledMultiColor(min, max, IM_COL32(8, 12, 19, 255), IM_COL32(16, 24, 40, 255), IM_COL32(7, 10, 17, 255), IM_COL32(12, 18, 32, 255));
    drawList->AddCircleFilled(ImVec2(min.x + 180.0f, min.y + 140.0f), 120.0f, IM_COL32(61, 110, 180, 28));
    drawList->AddCircleFilled(ImVec2(max.x - 180.0f, max.y - 160.0f), 150.0f, IM_COL32(255, 188, 102, 18));

    const ImVec2 cardSize(std::min(920.0f, viewport->WorkSize.x - 96.0f), std::min(600.0f, viewport->WorkSize.y - 72.0f));
    ImGui::SetCursorPos(ImVec2((viewport->WorkSize.x - cardSize.x) * 0.5f, (viewport->WorkSize.y - cardSize.y) * 0.5f));
    ImGui::BeginChild("welcome-card", cardSize, ImGuiChildFlags_Borders);

    ImGui::PushFont(m_headingFont ? m_headingFont : ImGui::GetFont());
    ImGui::TextUnformatted("Welcome to PrismatiX Editor");
    ImGui::PopFont();
    ImGui::TextDisabled("Choose where your project lives before opening the node editors. Assets, previews and exports will all follow that project folder.");
    ImGui::Spacing();

    auto sanitizeFolderName = [](std::string value) {
        if (value.empty()) {
            return std::string("MyNovelProject");
        }
        for (char& ch : value) {
            if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
                ch = '_';
            }
        }
        return value;
    };

    const fs::path locationPath = m_projectLocationInput.empty() ? DefaultProjectsRoot() : fs::path(m_projectLocationInput);
    const fs::path targetProjectPath = locationPath / fs::path(sanitizeFolderName(m_projectNameInput));

    ImGui::SeparatorText("Create Project");
    ImGui::InputText("Project Name", &m_projectNameInput);
    ImGui::InputText("Location", &m_projectLocationInput);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const fs::path selectedFolder = BrowseForFolder(locationPath, "Choose Project Parent Folder");
        if (!selectedFolder.empty()) {
            m_projectLocationInput = selectedFolder.string();
        }
    }

    ImGui::TextDisabled("Project Folder");
    ImGui::TextWrapped("%s", targetProjectPath.string().c_str());
    ImGui::Spacing();

    const bool canCreate = !m_projectNameInput.empty() && !m_projectLocationInput.empty();
    if (!canCreate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Create Project", ImVec2(180.0f, 44.0f))) {
        CreateProjectFromWelcome();
    }
    if (!canCreate) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Open Existing Folder", ImVec2(200.0f, 44.0f))) {
        const fs::path selectedFolder = BrowseForFolder(locationPath, "Open PrismatiX Project Folder");
        if (!selectedFolder.empty()) {
            OpenProjectFolder(selectedFolder);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Project Layout");
    ImGui::BulletText("Assets: images, audio and other imported resources");
    ImGui::BulletText("Scripts: scene scripts, Lua logic and generated exports");
    ImGui::BulletText("UI: UI-specific authored content");
    ImGui::BulletText("Everything in the resource browser comes from the active project");

    if (!m_logs.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Recent Activity");
        ImGui::BeginChild("welcome-activity", ImVec2(0.0f, 0.0f), false);
        const size_t startIndex = m_logs.size() > 8 ? m_logs.size() - 8 : 0;
        for (size_t index = startIndex; index < m_logs.size(); ++index) {
            ImGui::TextWrapped("%s", m_logs[index].c_str());
        }
        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::End();
}

bool EditorApp::CreateProjectFromWelcome() {
    if (m_projectNameInput.empty() || m_projectLocationInput.empty()) {
        Log("Project name and location are required.");
        return false;
    }

    std::string folderName = m_projectNameInput;
    for (char& ch : folderName) {
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }

    const fs::path rootPath = fs::path(m_projectLocationInput) / fs::path(folderName);
    return OpenProjectFolder(rootPath, m_projectNameInput);
}

bool EditorApp::OpenProjectFolder(const fs::path& rootPath, const std::string& preferredName) {
    if (rootPath.empty()) {
        return false;
    }

    const fs::path projectRoot = fs::absolute(rootPath);
    std::string projectName = preferredName.empty() ? projectRoot.filename().string() : preferredName;
    if (projectName.empty()) {
        projectName = "PrismatiXProject";
    }

    const bool createFreshManifest = !fs::exists(projectRoot / ".prismatix-project.json");
    if (!EnsureProjectScaffold(projectRoot, projectName, createFreshManifest)) {
        return false;
    }

    m_projectRoot = projectRoot;
    m_projectName = projectName;
    m_projectLocationInput = projectRoot.parent_path().string();
    m_projectNameInput = projectName;
    m_selectedAsset.clear();
    m_resourceFilter.clear();
    m_layoutBuilt = false;
    m_activeTab = WorkspaceTab::Entrypoint;
    m_lastDocumentTab = WorkspaceTab::Entrypoint;
    UpdateWindowTitle();
    Log("Project opened: " + m_projectRoot.string());
    return true;
}

bool EditorApp::EnsureProjectScaffold(const fs::path& rootPath, const std::string& projectName, bool createFreshManifest) {
    try {
        fs::create_directories(rootPath / "Assets");
        fs::create_directories(rootPath / "Scripts" / "Generated");
        fs::create_directories(rootPath / "Scripts" / "scenes");
        fs::create_directories(rootPath / "UI");

        const fs::path manifestPath = rootPath / ".prismatix-project.json";
        if (createFreshManifest || !fs::exists(manifestPath)) {
            std::string manifestName = projectName;
            std::replace(manifestName.begin(), manifestName.end(), '"', '\'');
            std::ofstream manifest(manifestPath, std::ios::binary);
            if (!manifest.is_open()) {
                Log("Failed to create project manifest: " + manifestPath.string());
                return false;
            }

            manifest << "{\n";
            manifest << "  \"name\": \"" << manifestName << "\",\n";
            manifest << "  \"format\": 1,\n";
            manifest << "  \"engine\": \"PrismatiXEditor\"\n";
            manifest << "}\n";
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
    m_activeTab = WorkspaceTab::Entrypoint;
    m_lastDocumentTab = WorkspaceTab::Entrypoint;
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
    RenderPreview();
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
            if (ImGui::MenuItem("New Project...")) {
                CloseProject();
            }
            if (ImGui::MenuItem("Open Project Folder...")) {
                const fs::path selectedFolder = BrowseForFolder(m_projectRoot.empty() ? DefaultProjectsRoot() : m_projectRoot, "Open PrismatiX Project Folder");
                if (!selectedFolder.empty()) {
                    OpenProjectFolder(selectedFolder);
                }
            }
            if (HasProjectLoaded() && ImGui::MenuItem("Close Project")) {
                CloseProject();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export Active")) {
                ExportActive();
            }
            if (ImGui::MenuItem("Export All")) {
                ExportAll();
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

        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("Blueprint-inspired scene and UI authoring for PrismatiX.");
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
    ImGuiID leftId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.20f, nullptr, &centerId);
    ImGuiID rightId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.25f, nullptr, &centerId);
    ImGuiID bottomId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Down, 0.30f, nullptr, &centerId);
    ImGuiID previewId = ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Down, 0.50f, nullptr, &rightId);

    ImGui::DockBuilderDockWindow("Workspace", centerId);
    ImGui::DockBuilderDockWindow("Resources", leftId);
    ImGui::DockBuilderDockWindow("Inspector", rightId);
    ImGui::DockBuilderDockWindow("Preview", previewId);
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

        const HRESULT showResult = dialog->Show(nullptr);
        if (SUCCEEDED(showResult)) {
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

std::vector<fs::path> EditorApp::ExplorerRoots() const {
    std::vector<fs::path> roots;
    if (!m_projectRoot.empty()) {
        for (const fs::path& candidate : {m_projectRoot / "Assets", m_projectRoot / "Scripts", m_projectRoot / "UI"}) {
            if (fs::exists(candidate)) {
                roots.push_back(candidate);
            }
        }
        if (roots.empty() && fs::exists(m_projectRoot)) {
            roots.push_back(m_projectRoot);
        }
        return roots;
    }

    return {};
}

std::string EditorApp::CurrentSelectedResource() const {
    if (m_selectedAsset.empty()) {
        return {};
    }
    return ToRuntimePath(m_selectedAsset);
}

EditorApp::WorkspaceTab EditorApp::CurrentDocumentTab() const {
    return m_activeTab == WorkspaceTab::Export ? m_lastDocumentTab : m_activeTab;
}

std::string EditorApp::WorkspaceTabLabel(WorkspaceTab tab) const {
    switch (tab) {
        case WorkspaceTab::Entrypoint:
            return "Entrypoint";
        case WorkspaceTab::SceneScript:
            return "Scene Script";
        case WorkspaceTab::UIDesign:
            return "UI Designer";
        case WorkspaceTab::Export:
            return "Export Manager";
    }

    return "Workspace";
}

fs::path EditorApp::ExportPathFor(WorkspaceTab tab) const {
    const WorkspaceTab resolvedTab = tab == WorkspaceTab::Export ? CurrentDocumentTab() : tab;
    const fs::path outputDir = (m_projectRoot.empty() ? (m_workspaceRoot / "PrismatiXEngine" / "Scripts" / "Generated") : (m_projectRoot / "Scripts" / "Generated"));
    switch (resolvedTab) {
        case WorkspaceTab::Entrypoint:
            return outputDir / "editor_entrypoint.lua";
        case WorkspaceTab::SceneScript:
            return outputDir / "editor_scene_graph.pds";
        case WorkspaceTab::UIDesign:
            return outputDir / "editor_ui_layout.lua";
        case WorkspaceTab::Export:
            break;
    }

    return outputDir;
}

std::string EditorApp::PreviewLabelFor(WorkspaceTab tab) const {
    const WorkspaceTab resolvedTab = tab == WorkspaceTab::Export ? CurrentDocumentTab() : tab;
    return resolvedTab == WorkspaceTab::SceneScript ? "PDS Preview" : "Lua Preview";
}

std::string EditorApp::GenerateDocumentFor(WorkspaceTab tab) const {
    const WorkspaceTab resolvedTab = tab == WorkspaceTab::Export ? CurrentDocumentTab() : tab;
    switch (resolvedTab) {
        case WorkspaceTab::Entrypoint:
            return m_entrypointEditor.GenerateLua();
        case WorkspaceTab::SceneScript:
            return m_sceneEditor.GenerateLua();
        case WorkspaceTab::UIDesign:
            return m_uiDesigner.GenerateLua();
        case WorkspaceTab::Export:
            break;
    }

    return {};
}

std::vector<EditorApp::ExportArtifact> EditorApp::BuildExportArtifacts() const {
    std::vector<ExportArtifact> artifacts;
    for (const WorkspaceTab tab : {WorkspaceTab::Entrypoint, WorkspaceTab::SceneScript, WorkspaceTab::UIDesign}) {
        ExportArtifact artifact;
        artifact.tab = tab;
        artifact.label = WorkspaceTabLabel(tab);
        artifact.previewLabel = PreviewLabelFor(tab);
        artifact.path = ExportPathFor(tab);
        artifact.content = GenerateDocumentFor(tab);
        artifacts.push_back(std::move(artifact));
    }

    return artifacts;
}

std::string EditorApp::ToRuntimePath(const fs::path& path) const {
    const fs::path sourceRoot = m_projectRoot.empty() ? (m_workspaceRoot / "PrismatiXEngine") : m_projectRoot;
    std::error_code ec;
    fs::path relative = fs::relative(path, sourceRoot, ec);
    if (ec) {
        relative = fs::relative(path, m_workspaceRoot, ec);
    }

    std::string runtimePath = relative.string();
    std::replace(runtimePath.begin(), runtimePath.end(), '\\', '/');
    return runtimePath;
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
    const std::string extension = path.extension().string();
    std::string lowered = extension;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered == ".png" || lowered == ".jpg" || lowered == ".jpeg" || lowered == ".bmp" || lowered == ".webp";
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

void EditorApp::Log(const std::string& message) {
    m_logs.push_back(message);
    if (m_logs.size() > 200) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + 50);
    }
    PX_LOG_INFO("{}", message);
}

}  // namespace PrismatiX::Editor
