#include "Editor/App/EditorApp.h"

#include "Editor/Services/AssetMeta.h"
#include "Logger.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
//
#include <shellapi.h>

#endif

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace px::editor {

namespace {
const std::array<const char*, 8> kAssetTypes = { "all", "image", "audio", "script", "ui", "font", "lua", "other" };
}

EditorApp::EditorApp() : m_project([this](const std::string& m) { Log(m); }), m_assets([this](const std::string& m) { Log(m); }), m_scripts([this](const std::string& m) { Log(m); }) {}

void EditorApp::Log(const std::string& message) {
    m_console.push_back(message);
    if (m_console.size() > 500) {
        m_console.erase(m_console.begin(), m_console.begin() + (m_console.size() - 500));
    }
    PX_LOG_INFO("[editor] {}", message);
}

bool EditorApp::Init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        PX_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    if (!TTF_Init()) {
        PX_LOG_ERROR("TTF_Init failed: {}", SDL_GetError());
        return false;
    }
    if (!m_window.Create("PrismatiX Editor", 1600, 960, true)) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (const char* base = SDL_GetBasePath()) {
        m_basePath = base;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    m_iniPath = m_basePath + "PrismatiXEditor.ini";
    io.IniFilename = m_iniPath.c_str();
    m_buildLayout = !std::filesystem::exists(m_iniPath);
    LoadFonts();
    ApplyTheme();

    if (!ImGui_ImplSDL3_InitForSDLRenderer(m_window.Handle(), m_window.Renderer())) {
        PX_LOG_ERROR("ImGui SDL3 init failed");
        return false;
    }
    if (!ImGui_ImplSDLRenderer3_Init(m_window.Renderer())) {
        PX_LOG_ERROR("ImGui SDLRenderer3 init failed");
        return false;
    }
    m_imguiReady = true;
    m_running = true;

    m_preview = std::make_unique<RuntimeHost>(m_window.Renderer());
    m_textures = std::make_unique<EditorTextures>(m_window.Renderer());
    m_nodeHeaderTex = m_textures->LoadId(m_basePath + "EditorAssets/NodeHeader.png", &m_nodeHeaderW, &m_nodeHeaderH);
    m_nodeEditor.SetHeaderTexture(m_nodeHeaderTex, m_nodeHeaderW, m_nodeHeaderH);
    m_nodeEditor.SetSelectedResourceCallback([this] { return m_selectedAsset; });

    const std::string cwd = std::filesystem::current_path().string();
    std::snprintf(m_openPath, sizeof(m_openPath), "%s", cwd.c_str());
    std::snprintf(m_newPath, sizeof(m_newPath), "%s", cwd.c_str());
    std::snprintf(m_newName, sizeof(m_newName), "%s", "MyGame");
    BuildCommands();
    Log("PrismatiX Editor started.");
    return true;
}

void EditorApp::OpenWorkspace() {
    OpenProject(std::filesystem::current_path());
    m_screen = Screen::Workspace;
}

void EditorApp::OpenProject(const std::filesystem::path& root) {
    if (m_project.Open(root)) {
        m_assets.Scan(m_project.Context());
        if (m_preview) {
            m_preview->SetProjectRoot(root.string());
            m_preview->LoadUi(m_project.Context().manifest.startUi);
        }
        m_nodeEditor.SetProject(&m_project.Context());
        m_nodeEditor.OpenDocument(m_project.Context().manifest.startScript);

        m_dbPath = (root / "Data" / "database.json").string();
        if (std::ifstream dbin(m_dbPath); dbin) {
            std::stringstream ss;
            ss << dbin.rdbuf();
            if (!m_database.Load(ss.str())) {
                m_database.SeedDefault();
            }
        }
        else {
            m_database.SeedDefault();
            std::ofstream out(m_dbPath);
            out << m_database.Serialize();
            Log("Created Data/database.json");
        }
        m_dbDirty = false;

        m_flow.SetOpenCallback([this](const std::string& script) { m_nodeEditor.OpenDocument(script); });
        m_flow.Rebuild(m_database, root);

        m_scripts.SetOnCommandsChanged([this](const std::vector<CustomCommandDef>& cmds) { m_nodeEditor.SetCustomCommands(cmds); });
        m_scripts.SetProject(&m_project.Context());

        RefreshProblems();
    }
}

void EditorApp::Run() {
    while (m_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                m_running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        BuildUi();

        ImGui::Render();
        m_window.Clear(Color{ 18, 20, 26, 255 });
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_window.Renderer());
        m_window.Present();
    }
}

void EditorApp::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string candidates[] = { m_basePath + "EditorAssets/UIFont.ttf", "Data/Font/NotoSansTC-Bold.ttf", "../../../Data/Font/NotoSansTC-Bold.ttf" };
    ImFont* loaded = nullptr;
    for (const std::string& path : candidates) {
        if (std::filesystem::exists(path)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());
            if (loaded) {
                Log("UI font: " + path);
                break;
            }
        }
    }
    if (!loaded) {
        io.Fonts->AddFontDefault();
        Log("UI font: built-in (UIFont.ttf not found).");
    }
}

void EditorApp::ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    s.WindowRounding = 7.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(9, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 10.0f;
    s.TabBarBorderSize = 0.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* c = s.Colors;
    const ImVec4 bg0(0.043f, 0.047f, 0.059f, 1.00f);
    const ImVec4 bg1(0.071f, 0.078f, 0.094f, 1.00f);
    const ImVec4 bg2(0.106f, 0.118f, 0.141f, 1.00f);
    const ImVec4 bg3(0.149f, 0.165f, 0.196f, 1.00f);
    const ImVec4 accent(0.247f, 0.553f, 0.949f, 1.00f);
    const ImVec4 accentDim(0.180f, 0.380f, 0.660f, 1.00f);
    const ImVec4 text(0.870f, 0.890f, 0.920f, 1.00f);
    const ImVec4 textDim(0.470f, 0.500f, 0.555f, 1.00f);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = bg0;
    c[ImGuiCol_Border] = ImVec4(0.16f, 0.18f, 0.22f, 0.9f);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive] = bg3;
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = accentDim;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = accentDim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = bg2;
    c[ImGuiCol_HeaderHovered] = bg3;
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Separator] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_Tab] = bg1;
    c[ImGuiCol_TabHovered] = accentDim;
    c[ImGuiCol_TabSelected] = bg3;
    c[ImGuiCol_TabDimmed] = bg1;
    c[ImGuiCol_TabDimmedSelected] = bg2;
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_PlotHistogram] = accent;
}

void EditorApp::BuildDockLayout(unsigned int dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Assets", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Database", right);
    ImGui::DockBuilderDockWindow("Node Editor", center);
    ImGui::DockBuilderDockWindow("Flow", center);
    ImGui::DockBuilderDockWindow("Scripting", center);
    ImGui::DockBuilderDockWindow("Preview", center);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Problems", bottom);
    ImGui::DockBuilderDockWindow("PDS Text", bottom);
    ImGui::DockBuilderDockWindow("Build", bottom);
    ImGui::DockBuilderDockWindow("Animation", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorApp::RenderWelcome() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.027f, 0.031f, 0.039f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const ImVec2 panel(560, 420);
    ImGui::SetCursorPos(ImVec2((vp->Size.x - panel.x) * 0.5f, (vp->Size.y - panel.y) * 0.5f));
    ImGui::BeginChild("welcomePanel", panel, ImGuiChildFlags_Borders);

    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextColored(ImVec4(0.247f, 0.553f, 0.949f, 1.0f), "PrismatiX");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Visual Novel Engine");
    ImGui::Dummy(ImVec2(0, 16));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 12));

    const ImVec2 btn(panel.x - 24, 42);
    if (ImGui::BeginTabBar("welcomeTabs")) {
        if (ImGui::BeginTabItem("Open")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextDisabled("Project folder");
            ImGui::SetNextItemWidth(panel.x - 24);
            ImGui::InputText("##path", m_openPath, sizeof(m_openPath));
            ImGui::Dummy(ImVec2(0, 10));
            if (ImGui::Button("Open Project", btn)) {
                OpenProject(std::filesystem::path(m_openPath));
                m_screen = Screen::Workspace;
            }
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::Button("Open Current Folder", btn)) {
                OpenProject(std::filesystem::current_path());
                m_screen = Screen::Workspace;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("New Project")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextDisabled("Project name");
            ImGui::SetNextItemWidth(panel.x - 24);
            ImGui::InputText("##name", m_newName, sizeof(m_newName));
            ImGui::TextDisabled("Location (a subfolder with the name is created here)");
            ImGui::SetNextItemWidth(panel.x - 24);
            ImGui::InputText("##newpath", m_newPath, sizeof(m_newPath));
            ImGui::Dummy(ImVec2(0, 10));
            if (ImGui::Button("Create Project", btn)) {
                const std::filesystem::path root = std::filesystem::path(m_newPath) / m_newName;
                if (m_project.Create(root, m_newName, m_basePath + "EditorAssets/UIFont.ttf")) {
                    OpenProject(root);
                    m_screen = Screen::Workspace;
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Dummy(ImVec2(0, 14));
    if (ImGui::Button("Quit", btn)) {
        m_running = false;
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void EditorApp::BuildUi() {
    if (m_screen == Screen::Welcome) {
        RenderWelcome();
        return;
    }
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
    if (m_buildLayout || dockNode == nullptr || dockNode->IsLeafNode()) {
        BuildDockLayout(dockId);
        m_buildLayout = false;
    }
    HandleShortcuts();
    SyncDesigner();
    RenderMenuBar();
    RenderCommandPalette();
    RenderHierarchy();
    RenderInspector();
    RenderAssets();
    RenderConsole();
    RenderPreview();
    RenderNodeEditor();
    RenderPdsText();
    RenderBuild();
    RenderDatabase();
    RenderAnimation();
    RenderFlow();
    RenderScripting();
    RenderProblems();
}

void EditorApp::SyncDesigner() {
    if (!m_preview) {
        return;
    }
    const bool uiMode = m_previewMode == 0;
    m_preview->UiStageRef().SetEditMode(uiMode && !m_previewAnims);
    if (!uiMode) {
        return;
    }
    const std::string abs = (m_project.Context().root / m_preview->CurrentUiPath()).string();
    if (m_designerPath != abs) {
        m_designer.SetScene(&m_preview->UiStageRef().Scene(), abs);
        m_designer.SetOnStructureChange([this] { m_preview->UiStageRef().OnSceneEdited(); });
        m_designerPath = abs;
    }
}

void EditorApp::RenderMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Current Folder")) {
            OpenProject(std::filesystem::current_path());
        }
        if (ImGui::MenuItem("Rescan Assets")) {
            m_assets.Scan(m_project.Context());
        }
        if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
            if (m_project.SaveManifest()) Log("Manifest saved.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            m_running = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem(("Undo " + m_undo.NextUndoLabel()).c_str(), "Ctrl+Z", false, m_undo.CanUndo())) {
            m_undo.Undo();
        }
        if (ImGui::MenuItem(("Redo " + m_undo.NextRedoLabel()).c_str(), "Ctrl+Y", false, m_undo.CanRedo())) {
            m_undo.Redo();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Command Palette", "Ctrl+P")) {
            m_paletteOpen = true;
            m_paletteFocus = true;
            m_paletteFilter[0] = 0;
        }
        if (ImGui::MenuItem("Reset Layout")) {
            m_buildLayout = true;
        }
        if (ImGui::MenuItem("Back to Welcome")) {
            m_screen = Screen::Welcome;
        }
        ImGui::EndMenu();
    }

    ImGui::TextDisabled("   |   ");
    if (ImGui::SmallButton("Save All")) {
        SaveAll();
    }
    if (ImGui::SmallButton("Reload Preview") && m_preview) {
        m_preview->Reload();
    }
    if (ImGui::SmallButton("Run")) {
        RunDev();
    }
    if (ImGui::SmallButton("Build")) {
        RunBuild();
    }
    ImGui::EndMainMenuBar();
}

void EditorApp::RenderHierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        if (m_previewMode == 0) {
            m_designer.RenderHierarchy();
        }
        else {
            ImGui::TextDisabled("Switch Preview to UI Scene mode to edit a .pxui.");
        }
    }
    ImGui::End();
}

void EditorApp::RenderInspector() {
    if (ImGui::Begin("Inspector")) {
        if (m_previewMode == 0) {
            m_designer.RenderInspector(m_selectedAsset);
        }
        else {
            m_nodeEditor.RenderInspector();
            if (m_textures && m_selectedAsset.size() > 4) {
                const std::string ext = m_selectedAsset.substr(m_selectedAsset.size() - 4);
                if (ext == ".png" || ext == ".jpg" || ext == "jpeg" || ext == "webp") {
                    const std::string abs = (m_project.Context().root / m_selectedAsset).string();
                    int w = 0, h = 0;
                    if (ImTextureID t = m_textures->LoadId(abs, &w, &h); t && w > 0) {
                        const float avail = ImGui::GetContentRegionAvail().x;
                        const float scale = avail / static_cast<float>(w);
                        ImGui::Image(t, ImVec2(avail, h * scale));
                        ImGui::TextDisabled("%dx%d", w, h);
                    }
                }
            }
        }
    }
    ImGui::End();
}

void EditorApp::RenderAssets() {
    if (ImGui::Begin("Assets")) {
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##filter", "filter...", m_assetFilter, sizeof(m_assetFilter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##type", kAssetTypes[m_assetTypeIndex])) {
            for (int i = 0; i < static_cast<int>(kAssetTypes.size()); ++i) {
                if (ImGui::Selectable(kAssetTypes[i], m_assetTypeIndex == i)) m_assetTypeIndex = i;
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        const bool isPds = m_selectedAsset.size() > 4 && m_selectedAsset.substr(m_selectedAsset.size() - 4) == ".pds";
        if (isPds && ImGui::Button("Open in Node Editor")) {
            m_nodeEditor.OpenDocument(m_selectedAsset);
        }

        if (!m_selectedAsset.empty() && m_project.Context().IsOpen()) {
            const std::filesystem::path abs = m_project.Context().root / m_selectedAsset;
            if (m_metaAsset != m_selectedAsset) {
                m_meta = LoadAssetMeta(abs);
                m_metaAsset = m_selectedAsset;
            }
            if (ImGui::CollapsingHeader("Import settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool changed = ImGui::Checkbox("Include in build", &m_meta.includeInBuild);
                changed |= ImGui::InputText("Note", &m_meta.note);
                if (changed) SaveAssetMeta(abs, m_meta);
            }
        }
        ImGui::Separator();

        const auto filtered = m_assets.Filter(m_assetFilter, kAssetTypes[m_assetTypeIndex]);
        ImGui::Text("%zu assets", filtered.size());
        if (ImGui::BeginChild("##assetlist")) {
            for (const AssetRecord& rec : filtered) {
                ImGui::PushID(rec.runtimePath.c_str());
                if (rec.type == "image" && m_textures) {
                    if (ImTextureID thumb = m_textures->LoadId(rec.absolutePath.string())) {
                        ImGui::Image(thumb, ImVec2(40, 24));
                        ImGui::SameLine();
                    }
                }
                const bool selected = rec.runtimePath == m_selectedAsset;
                if (ImGui::Selectable(rec.runtimePath.c_str(), selected)) {
                    m_selectedAsset = rec.runtimePath;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::RenderConsole() {
    if (ImGui::Begin("Console")) {
        if (ImGui::SmallButton("Clear")) m_console.clear();
        ImGui::Separator();
        if (ImGui::BeginChild("##log")) {
            for (const std::string& line : m_console) {
                ImGui::TextUnformatted(line.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::RenderPreview() {
    if (ImGui::Begin("Preview") && m_preview) {
        ImGui::SetNextItemWidth(140);
        const char* modes[] = { "UI Scene", "VN Script" };
        if (ImGui::Combo("##mode", &m_previewMode, modes, 2)) {
            if (m_previewMode == 0) {
                m_preview->LoadUi(m_project.Context().manifest.startUi);
            }
            else {
                m_preview->LoadVn(m_project.Context().manifest.startScript);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload")) {
            m_preview->Reload();
        }
        if (m_previewMode == 0) {
            ImGui::SameLine();
            const std::string current = m_preview->CurrentUiPath();
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo("##screen", current.empty() ? "(screen)" : current.c_str())) {
                for (const AssetRecord& a : m_assets.Filter("", "ui")) {
                    if (ImGui::Selectable(a.runtimePath.c_str(), a.runtimePath == current)) {
                        m_preview->LoadUi(a.runtimePath);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Screen")) {
                m_designer.Save();
            }
            ImGui::SameLine();
            if (ImGui::Button("New Screen")) {
                ImGui::OpenPopup("newScreen");
            }
            if (ImGui::BeginPopup("newScreen")) {
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("name", m_newScreenName, sizeof(m_newScreenName));
                ImGui::SameLine();
                if (ImGui::Button("Create")) {
                    const std::filesystem::path rel = std::filesystem::path("Data/UI") / (std::string(m_newScreenName) + ".pxui");
                    const std::filesystem::path abs = m_project.Context().root / rel;
                    std::error_code ec;
                    std::filesystem::create_directories(abs.parent_path(), ec);
                    if (!std::filesystem::exists(abs)) {
                        std::ofstream out(abs);
                        out << "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
                               "  \"background\": { \"color\": [12,14,20,255] },\n  \"nodes\": []\n}\n";
                    }
                    m_assets.Scan(m_project.Context());
                    m_preview->LoadUi(rel.generic_string());
                    Log("Created screen " + rel.generic_string());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float gw = static_cast<float>(m_preview->Width());
        const float gh = static_cast<float>(m_preview->Height());
        const float scale = std::min(avail.x / gw, avail.y / gh);
        const ImVec2 disp(gw * scale, gh * scale);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        p0.x += (avail.x - disp.x) * 0.5f;
        p0.y += std::max(0.0f, (avail.y - disp.y) * 0.5f);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hovered = mouse.x >= p0.x && mouse.x <= p0.x + disp.x && mouse.y >= p0.y && mouse.y <= p0.y + disp.y;
        const float localX = scale > 0 ? (mouse.x - p0.x) / scale : 0.0f;
        const float localY = scale > 0 ? (mouse.y - p0.y) / scale : 0.0f;
        const bool click = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool uiMode = m_previewMode == 0;

        m_preview->Tick(ImGui::GetIO().DeltaTime, SDL_GetTicks(), hovered, localX, localY, uiMode ? false : click);

        if (m_preview->Target()) {
            ImGui::SetCursorScreenPos(p0);
            ImGui::Image(reinterpret_cast<ImTextureID>(m_preview->Target()), disp);
        }
        if (uiMode) {
            m_designer.CanvasInput(p0, scale, hovered);
            m_designer.DrawOverlay(p0, scale);
        }
    }
    ImGui::End();
}

void EditorApp::RenderNodeEditor() {
    if (ImGui::Begin("Node Editor")) {
        m_nodeEditor.Render();
    }
    ImGui::End();
}

void EditorApp::RenderPdsText() {
    if (ImGui::Begin("PDS Text")) {
        std::string text = m_nodeEditor.Compile();
        ImGui::InputTextMultiline("##pds", &text, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::End();
}

void EditorApp::SaveAll() {
    if (m_designer.Dirty()) m_designer.Save();
    if (m_nodeEditor.Dirty()) m_nodeEditor.Save();
    m_project.SaveManifest();
    Log("Saved all documents.");
}

void EditorApp::RunBuild() {
    if (!m_project.Context().IsOpen()) {
        Log("Build: no project open.");
        return;
    }
    ProjectManifest& m = m_project.Context().manifest;
    BuildService builder([this](const std::string& s) { Log(s); });
    BuildOptions opt;
    opt.projectRoot = m_project.Context().root;
    opt.outputDir = m_project.Context().ExportRoot();
    if (const char* base = SDL_GetBasePath()) {
        opt.playerExe = std::filesystem::path(base) / "PrismatiXPlayer.exe";
    }
    opt.title = m.name;
    opt.startUi = m.startUi;
    opt.startScript = m.startScript;
    opt.key = m.encryptKey;
    opt.encrypt = m.encrypt;
    opt.gameWidth = m.gameWidth;
    opt.gameHeight = m.gameHeight;
    builder.Build(opt);
}

void EditorApp::RunPlayer(const std::filesystem::path& exe, const std::filesystem::path& workingDir) {
    if (!std::filesystem::exists(exe)) {
        Log("Run failed: player not found at " + exe.string());
        return;
    }
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    const std::wstring cwd = workingDir.wstring();
    if (CreateProcessW(exe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Log("Launched player (cwd=" + workingDir.string() + ")");
    }
    else {
        Log("Run failed: could not start the player process.");
    }
#else
    Log("Run player is only implemented on Windows in this build.");
#endif
}

void EditorApp::RunDev() {
    if (!m_project.Context().IsOpen()) {
        Log("Run: no project open.");
        return;
    }
    SaveAll();
    RunPlayer(std::filesystem::path(m_basePath) / "PrismatiXPlayer.exe", m_project.Context().root);
}

void EditorApp::RunPackaged() {
    const std::filesystem::path exportRoot = m_project.Context().ExportRoot();
    RunPlayer(exportRoot / "PrismatiXPlayer.exe", exportRoot);
}

void EditorApp::OpenInExplorer(const std::filesystem::path& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    Log("Open: " + path.string());
}

void EditorApp::RenderAnimation() {
    if (ImGui::Begin("Animation")) {
        if (m_previewMode != 0) {
            ImGui::TextDisabled("Switch Preview to UI Scene mode to edit animations.");
        }
        else {
            if (ImGui::Checkbox("Preview animations (play in canvas)", &m_previewAnims)) {
                if (m_previewAnims && m_preview) {
                    m_preview->UiStageRef().TriggerEnter();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Replay") && m_preview) {
                m_preview->UiStageRef().TriggerEnter();
            }
            ImGui::Separator();
            m_designer.RenderAnimation();
        }
    }
    ImGui::End();
}

void EditorApp::RenderScripting() {
    if (ImGui::Begin("Scripting")) {
        m_scripts.Render();
    }
    ImGui::End();
}

void EditorApp::BuildCommands() {
    const auto focus = [](const char* name) { ImGui::SetWindowFocus(name); };
    m_commands = {
        { "Save All", "Ctrl+S", [this] { SaveAll(); } },
        { "Run (Dev)", "F5", [this] { RunDev(); } },
        { "Build", "Ctrl+B", [this] { RunBuild(); } },
        { "Build & Run",
          "",
          [this] {
              RunBuild();
              RunPackaged();
          } },
        { "Open Output Folder",
          "",
          [this] {
              std::error_code ec;
              std::filesystem::create_directories(m_project.Context().ExportRoot(), ec);
              OpenInExplorer(m_project.Context().ExportRoot());
          } },
        { "Reload Preview",
          "",
          [this] {
              if (m_preview) m_preview->Reload();
          } },
        { "Rescan Assets", "", [this] { m_assets.Scan(m_project.Context()); } },
        { "Refresh Problems", "", [this] { RefreshProblems(); } },
        { "Reset Layout", "", [this] { m_buildLayout = true; } },
        { "Back to Welcome", "", [this] { m_screen = Screen::Welcome; } },
        { "Undo", "Ctrl+Z", [this] { m_undo.Undo(); } },
        { "Redo", "Ctrl+Y", [this] { m_undo.Redo(); } },
        { "Go to: Preview", "", [focus] { focus("Preview"); } },
        { "Go to: Story (Node Editor)", "", [focus] { focus("Node Editor"); } },
        { "Go to: Flow", "", [focus] { focus("Flow"); } },
        { "Go to: Scripting", "", [focus] { focus("Scripting"); } },
        { "Go to: Database", "", [focus] { focus("Database"); } },
        { "Go to: Animation", "", [focus] { focus("Animation"); } },
        { "Go to: Build", "", [focus] { focus("Build"); } },
        { "Go to: Assets", "", [focus] { focus("Assets"); } },
        { "Go to: Problems", "", [focus] { focus("Problems"); } },
    };
}

void EditorApp::HandleShortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal)) {
        m_paletteOpen = true;
        m_paletteFocus = true;
        m_paletteFilter[0] = 0;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        SaveAll();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal)) {
        RunBuild();
    }
    if (ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal)) {
        RunDev();
    }
}

void EditorApp::RenderCommandPalette() {
    if (m_paletteOpen) {
        ImGui::OpenPopup("##cmdPalette");
        m_paletteOpen = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->Pos.y + 120), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##cmdPalette")) {
        if (m_paletteFocus) {
            ImGui::SetKeyboardFocusHere();
            m_paletteFocus = false;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##cmdfilter", "Type a command...", m_paletteFilter, sizeof(m_paletteFilter));
        ImGui::Separator();

        std::string filter = m_paletteFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto matches = [&](const std::string& label) {
            if (filter.empty()) return true;
            std::string low = label;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return low.find(filter) != std::string::npos;
        };

        bool runFirst = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        bool ranOne = false;
        if (ImGui::BeginChild("##cmdlist", ImVec2(0, 300))) {
            for (const PaletteCommand& cmd : m_commands) {
                if (!matches(cmd.label)) continue;
                const std::string row = cmd.shortcut.empty() ? cmd.label : (cmd.label + "\t" + cmd.shortcut);
                const bool clicked = ImGui::Selectable(row.c_str());
                if (clicked || (runFirst && !ranOne)) {
                    if (cmd.run) cmd.run();
                    ranOne = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::RefreshProblems() {
    m_problems.clear();
    if (!m_project.Context().IsOpen()) {
        return;
    }
    const std::filesystem::path root = m_project.Context().root;
    const ProjectManifest& m = m_project.Context().manifest;

    const auto missing = [&](const std::filesystem::path& rel, const std::string& what) {
        if (!std::filesystem::exists(root / rel)) {
            m_problems.push_back("Missing " + what + ": " + rel.generic_string());
        }
    };
    missing(m.startUi, "start UI");
    if (!std::filesystem::exists(root / m.startScript) && !std::filesystem::exists(root / "Data" / "Script" / m.startScript)) {
        m_problems.push_back("Missing start script: " + m.startScript);
    }
    for (const auto& ch : m_database.chapters) {
        if (ch.script.empty()) continue;
        if (!std::filesystem::exists(root / "Data" / "Script" / ch.script) && !std::filesystem::exists(root / ch.script)) {
            m_problems.push_back("Chapter '" + ch.id + "' script not found: " + ch.script);
        }
    }
    for (const auto& g : m_database.gallery) {
        if (!g.image.empty() && !std::filesystem::exists(root / g.image)) {
            m_problems.push_back("Gallery '" + g.id + "' image not found: " + g.image);
        }
    }
}

void EditorApp::RenderProblems() {
    if (ImGui::Begin("Problems")) {
        if (ImGui::SmallButton("Refresh")) RefreshProblems();
        ImGui::SameLine();
        if (m_problems.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.5f, 1.0f), "No problems detected.");
        }
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "%zu problem(s)", m_problems.size());
        }
        ImGui::Separator();
        if (ImGui::BeginChild("##problemlist")) {
            for (const std::string& p : m_problems) {
                ImGui::BulletText("%s", p.c_str());
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::RenderFlow() {
    if (ImGui::Begin("Flow")) {
        if (ImGui::SmallButton("Rebuild") && m_project.Context().IsOpen()) {
            m_flow.Rebuild(m_database, m_project.Context().root);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Story map — chapters from the Database + cross-script jumps.");
        ImGui::Separator();
        m_flow.Render();
    }
    ImGui::End();
}

void EditorApp::RenderDatabase() {
    if (ImGui::Begin("Database")) {
        if (ImGui::Button(m_dbDirty ? "Save*" : "Save") && !m_dbPath.empty()) {
            std::ofstream out(m_dbPath);
            out << m_database.Serialize();
            m_dbDirty = false;
            Log("Saved database.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Cast / Variables / Gallery / Chapters / Input triggers");
        ImGui::Separator();
        if (m_dbPanel.Render(m_database)) {
            m_dbDirty = true;
        }
    }
    ImGui::End();
}

void EditorApp::RenderBuild() {
    if (ImGui::Begin("Build")) {
        ProjectManifest& m = m_project.Context().manifest;

        ImGui::SeparatorText("Play");
        if (ImGui::Button("Run (Dev)", ImVec2(150, 34))) {
            RunDev();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Saves all, then runs the player from the project folder.");

        ImGui::SeparatorText("Build profile");
        const char* profiles[] = { "Debug (unencrypted)", "Release (encrypted)" };
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("##profile", &m_buildProfile, profiles, 2)) {
            m.encrypt = (m_buildProfile == 1);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Encrypt", &m.encrypt);
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Key", &m.encryptKey);

        ImGui::SeparatorText("Publish");
        if (ImGui::Button("Build", ImVec2(110, 34))) {
            RunBuild();
        }
        ImGui::SameLine();
        if (ImGui::Button("Build & Run", ImVec2(130, 34))) {
            RunBuild();
            RunPackaged();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Output", ImVec2(130, 34))) {
            std::error_code ec;
            std::filesystem::create_directories(m_project.Context().ExportRoot(), ec);
            OpenInExplorer(m_project.Context().ExportRoot());
        }
        ImGui::TextDisabled("-> %s", m_project.Context().ExportRoot().string().c_str());
        ImGui::TextWrapped(
            "Packs Data/ into Data.pdx (honoring per-asset 'Include in build'), bundles "
            "the player + DLLs, and writes game.prismatix. White-box key (deters casual "
            "extraction)."
        );
    }
    ImGui::End();
}

void EditorApp::Shutdown() {
    if (m_imguiReady) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    m_textures.reset();
    m_preview.reset();
    m_window.Destroy();
    TTF_Quit();
    SDL_Quit();
}

}  // namespace px::editor
