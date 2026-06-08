#include "Editor/Application/EditorApp.h"

#include "Editor/Assets/AssetMeta.h"
#include "Engine/Support/Logger.h"

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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace px::editor {

namespace {
const std::array<const char*, 8> kAssetTypes = { "all", "image", "audio", "script", "ui", "font", "lua", "other" };

namespace fs = std::filesystem;

fs::path PathFromUtf8(const char* text) {
    if (!text) {
        return {};
    }
#ifdef _WIN32
    const int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (wideSize > 0) {
        std::vector<wchar_t> wide(static_cast<size_t>(wideSize));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), wideSize);
        return fs::path(wide.data());
    }
#endif
    return fs::path(text);
}

fs::path ImportFolderForType(const std::string& type) {
    if (type == "image") return fs::path("Data") / "Image" / "Imported";
    if (type == "audio") return fs::path("Data") / "Audio" / "Imported";
    if (type == "script") return fs::path("Data") / "Script";
    if (type == "ui") return fs::path("Data") / "UI";
    if (type == "font") return fs::path("Data") / "Font";
    if (type == "lua") return fs::path("Data") / "Scripts";
    return fs::path("Data") / "Imported";
}

fs::path UniqueDestination(const fs::path& requested) {
    if (!fs::exists(requested)) {
        return requested;
    }
    const fs::path parent = requested.parent_path();
    const std::string stem = requested.stem().string();
    const std::string ext = requested.extension().string();
    for (int i = 1; i < 10000; ++i) {
        fs::path candidate = parent / (stem + "_" + std::to_string(i) + ext);
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }
    return parent / (stem + "_copy" + ext);
}

bool IsPathWithin(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    fs::path childAbs = fs::weakly_canonical(child, ec);
    if (ec) {
        ec.clear();
        childAbs = fs::absolute(child, ec);
    }
    ec.clear();
    fs::path parentAbs = fs::weakly_canonical(parent, ec);
    if (ec) {
        ec.clear();
        parentAbs = fs::absolute(parent, ec);
    }
    ec.clear();
    const fs::path rel = fs::relative(childAbs, parentAbs, ec);
    if (ec) {
        return false;
    }
    for (const fs::path& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string RuntimePath(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const fs::path rel = fs::relative(path, root, ec);
    return ec ? path.generic_string() : rel.generic_string();
}
}

EditorApp::EditorApp() : m_project([this](const std::string& m) { Log(m); }), m_assets([this](const std::string& m) { Log(m); }), m_scripts([this](const std::string& m) { Log(m); }) {}

void EditorApp::Log(const std::string& message) {
    m_console.push_back(message);
    if (m_console.size() > 500) {
        m_console.erase(m_console.begin(), m_console.begin() + (m_console.size() - 500));
    }
    PX_LOG_INFO("[editor] {}", message);
}

void EditorApp::ImportAssetFiles(const std::vector<std::filesystem::path>& paths) {
    const ProjectContext& context = m_project.Context();
    if (!context.IsOpen()) {
        Log("Open a project before importing assets.");
        return;
    }

    std::vector<fs::path> files;
    for (const fs::path& rawPath : paths) {
        if (rawPath.empty()) {
            continue;
        }
        std::error_code ec;
        const fs::path source = fs::absolute(rawPath, ec);
        const fs::path path = ec ? rawPath : source;
        if (!fs::exists(path, ec)) {
            Log("Asset import skipped, missing file: " + path.string());
            continue;
        }
        if (fs::is_directory(path, ec)) {
            for (fs::recursive_directory_iterator it(path, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                if (it->path().extension() == ".meta") {
                    continue;
                }
                files.push_back(it->path());
            }
        }
        else if (fs::is_regular_file(path, ec) && path.extension() != ".meta") {
            files.push_back(path);
        }
    }

    int imported = 0;
    int alreadyInProject = 0;
    std::string lastRuntimePath;
    for (const fs::path& file : files) {
        std::error_code ec;
        if (IsPathWithin(file, context.DataRoot())) {
            lastRuntimePath = RuntimePath(context.root, file);
            ++alreadyInProject;
            continue;
        }

        const std::string type = AssetDatabase::Classify(file);
        const fs::path destination =
            UniqueDestination(context.root / ImportFolderForType(type) / file.filename());
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            Log("Asset import failed to create folder: " + destination.parent_path().string());
            continue;
        }
        fs::copy_file(file, destination, fs::copy_options::none, ec);
        if (ec) {
            Log("Asset import failed: " + file.string());
            continue;
        }
        lastRuntimePath = RuntimePath(context.root, destination);
        ++imported;
    }

    if (imported == 0 && alreadyInProject == 0) {
        Log("No importable asset files found.");
        return;
    }

    m_assets.Scan(context);
    if (!lastRuntimePath.empty()) {
        m_selectedAsset = lastRuntimePath;
        m_metaAsset.clear();
    }
    if (imported > 0) {
        Log("Imported " + std::to_string(imported) + " asset(s).");
    }
    if (alreadyInProject > 0) {
        Log("Selected " + std::to_string(alreadyInProject) + " existing project asset(s).");
    }
}

void EditorApp::ImportClipboardAssets() {
#ifdef _WIN32
    std::vector<fs::path> paths;
    if (!OpenClipboard(nullptr)) {
        Log("Clipboard is not available.");
        return;
    }
    if (HANDLE data = GetClipboardData(CF_HDROP)) {
        HDROP drop = reinterpret_cast<HDROP>(data);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) {
                continue;
            }
            std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1);
            DragQueryFileW(drop, i, buffer.data(), len + 1);
            paths.emplace_back(buffer.data());
        }
    }
    CloseClipboard();

    if (paths.empty()) {
        Log("Clipboard has no copied files to import.");
        return;
    }
    ImportAssetFiles(paths);
#else
    Log("Clipboard file import is only implemented on Windows.");
#endif
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
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

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
    m_flow.SetHeaderTexture(m_nodeHeaderTex, m_nodeHeaderW, m_nodeHeaderH);
    m_nodeEditor.SetSelectedResourceCallback([this] { return m_selectedAsset; });

    const std::string cwd = std::filesystem::current_path().string();
    std::snprintf(m_openPath, sizeof(m_openPath), "%s", cwd.c_str());
    std::snprintf(m_newPath, sizeof(m_newPath), "%s", cwd.c_str());
    std::snprintf(m_newName, sizeof(m_newName), "%s", "MyGame");
    BuildCommands();
    Log("PrismatiX Editor started.");
    return true;
}


void EditorApp::Run() {
    while (m_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                ImportAssetFiles({ PathFromUtf8(event.drop.data) });
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
        m_window.Clear(Color{ 18, 20, 26, 255 });
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_window.Renderer());
        m_window.Present();
    }
}

void EditorApp::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string candidates[] = { m_basePath + "EditorAssets/UIFont.ttf", "Resources/Fonts/NotoSansTC-Bold.ttf", "../../../Resources/Fonts/NotoSansTC-Bold.ttf" };
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
