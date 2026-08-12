#include "Editor/Application/EditorApp.h"

#include "Engine/Support/Logger.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/IO/AtomicFile.h"

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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace px::editor {

namespace {
void PrepareImGuiRenderer(SDL_Renderer* renderer) {
    const ImGuiIO& io = ImGui::GetIO();
    const float scaleX = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const float scaleY = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;

    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderScale(renderer, scaleX, scaleY);
}
}

void DiagnosticToastQueue::Push(const diag::Diagnostic& diagnostic) {
    if (diagnostic.severity < diag::Severity::Warning) return;
    m_items.push_back({diagnostic, std::chrono::steady_clock::now()});
    while (m_items.size() > 5) m_items.pop_front();
}

void DiagnosticToastQueue::Prune(std::chrono::steady_clock::time_point now) {
    while (!m_items.empty() && now - m_items.front().created > std::chrono::seconds(8))
        m_items.pop_front();
}

EditorApp::EditorApp() : m_project([this](const std::string& m) { Log(m); }) {}

void EditorApp::Log(const std::string& message) {
    m_console.push_back(message);
    if (m_console.size() > 500) {
        m_console.erase(m_console.begin(), m_console.begin() + (m_console.size() - 500));
    }
    PX_LOG_INFO("[editor] {}", message);
}

bool EditorApp::Init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
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
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (const char* base = SDL_GetBasePath()) {
        m_basePath = base;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    std::filesystem::path settingsRoot;
#ifdef _WIN32
    char* local = nullptr;
    std::size_t localLength = 0;
    if (_dupenv_s(&local, &localLength, "LOCALAPPDATA") == 0 && local && *local) {
        settingsRoot = std::filesystem::path(local) / "PrismatiXEditor";
    }
    std::free(local);
#endif
    if (settingsRoot.empty()) settingsRoot = std::filesystem::path(m_basePath) / ".editor";
    std::error_code settingsError;
    std::filesystem::create_directories(settingsRoot, settingsError);
    m_iniPath = (settingsRoot / "EditorLayout-v4.ini").string();
    io.IniFilename = m_iniPath.c_str();
    m_buildLayout = !std::filesystem::exists(m_iniPath);
    LoadFonts();
    ImGui::StyleColorsDark();

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

    const std::string cwd = std::filesystem::current_path().string();
    std::snprintf(m_openPath, sizeof(m_openPath), "%s", cwd.c_str());
    std::snprintf(m_newPath, sizeof(m_newPath), "%s", cwd.c_str());
    std::snprintf(m_newName, sizeof(m_newName), "%s", "MyGame");
    LoadRecentProjects();
    BuildCommands();
    diag::Global().SetListener([this](const diag::Diagnostic& diagnostic) {
        m_console.push_back("[" + diagnostic.code + "] " + diagnostic.message);
        if (m_console.size() > 500) m_console.erase(m_console.begin(),m_console.begin()+static_cast<std::ptrdiff_t>(m_console.size()-500));
        m_toasts.Push(diagnostic);
    });
    Log("PrismatiX Editor started.");
    return true;
}

Status EditorApp::EnsureAssetIdentity(const std::filesystem::path& path){
    return EnsureAssetIdentities({path});
}

Status EditorApp::EnsureAssetIdentities(const std::vector<std::filesystem::path>& paths){
    if(!m_project.Context().IsOpen())return Status::Ok();
    for(const auto& path:paths){
        auto registered=m_assetRegistry.RegisterAsset(m_project.Context().root,path,ClassifyAsset(path));
        if(!registered)return Status::Fail(registered.Diagnostics());
    }
    const Status scanned=m_assetRegistry.Scan(m_project.Context().root);
    if(!scanned)m_showAssetIdentity=true;
    return Status::Ok();
}


void EditorApp::Run() {
    while (m_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
                m_project.Context().IsOpen()) {
                const Status scanned=m_assetRegistry.Scan(m_project.Context().root);
                if(!scanned)m_showAssetIdentity=true;
            }
            if (event.type == SDL_EVENT_QUIT) {
                m_running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        BuildUI();

        ImGui::Render();
        PrepareImGuiRenderer(m_window.Renderer());
        m_window.Clear(Color{ 18, 20, 26, 255 });
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_window.Renderer());
        m_window.Present();
    }
}

void EditorApp::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/msjh.ttc",
#endif
        m_basePath + "EditorAssets/UIFont.ttf",
        "Resources/Fonts/NotoSansTC-Regular.ttf",
        "Resources/Fonts/NotoSansTC-Bold.ttf",
        "../../../Resources/Fonts/NotoSansTC-Bold.ttf" };
    ImFont* loaded = nullptr;
    for (const std::string& path : candidates) {
        if (std::filesystem::exists(path)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());
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


void EditorApp::Shutdown() {
    diag::Global().SetListener({});
    if (m_imguiReady) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    m_window.Destroy();
    TTF_Quit();
    SDL_Quit();
}

}  // namespace px::editor
