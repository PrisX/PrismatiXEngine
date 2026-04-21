#include "GamePreviewHost.h"

#include <SDL2/SDL.h>

#include <fstream>

#include <sol/sol.hpp>

#include "Core/Services/ResourceManager.h"

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

namespace {
std::string ReadTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
}  // namespace

GamePreviewHost::GamePreviewHost(LogCallback logCallback)
    : m_logCallback(std::move(logCallback)) {}

GamePreviewHost::~GamePreviewHost() {
    Shutdown();
}

void GamePreviewHost::SetWorkspaceRoot(const fs::path& workspaceRoot) {
    m_workspaceRoot = workspaceRoot;
}

void GamePreviewHost::SetProjectRoot(const fs::path& projectRoot) {
    if (m_projectRoot == projectRoot) {
        return;
    }
    m_projectRoot = projectRoot;
    RequestReload();
}

void GamePreviewHost::SetPreviewDataRoot(const fs::path& previewDataRoot) {
    if (m_previewDataRoot == previewDataRoot) {
        return;
    }
    m_previewDataRoot = previewDataRoot;
    RequestReload();
}

void GamePreviewHost::SetScenePath(const std::string& scenePath) {
    if (scenePath.empty() || m_scenePath == scenePath) {
        return;
    }
    m_scenePath = scenePath;
    RequestReload();
}

void GamePreviewHost::SetStoryScriptPath(const std::string& scriptPath) {
    if (scriptPath.empty() || m_storyScriptPath == scriptPath) {
        return;
    }
    m_storyScriptPath = scriptPath;
    RequestReload();
}

void GamePreviewHost::SetViewportSize(int width, int height) {
    const int nextWidth = std::max(width, 16);
    const int nextHeight = std::max(height, 16);
    if (m_width == nextWidth && m_height == nextHeight) {
        return;
    }
    m_width = nextWidth;
    m_height = nextHeight;
    RequestReload();
}

void GamePreviewHost::RequestReload() {
    m_faulted = false;
    m_reloadRequested = true;
}

bool GamePreviewHost::InitializeRuntime(SDL_Window* window, SDL_Renderer* renderer) {
    Shutdown();

    if (!window || !renderer) {
        m_status = "Preview unavailable: renderer missing";
        m_faulted = true;
        m_reloadRequested = false;
        return false;
    }

    if (m_projectRoot.empty()) {
        m_status = "Preview unavailable: project not loaded";
        m_faulted = true;
        m_reloadRequested = false;
        return false;
    }

    PrismatiX::App::Engine::InitOptions options;
    options.externalWindow = window;
    options.externalRenderer = renderer;
    options.ownWindow = false;
    options.ownRenderer = false;
    options.ownSubsystems = false;
    options.applyRendererLogicalSize = false;

    if (!m_engine.Initialize("PrismatiX Preview", m_width, m_height, options)) {
        m_status = "Preview initialization failed";
        m_faulted = true;
        m_reloadRequested = false;
        return false;
    }
    m_engineInitialized = true;

    const fs::path dataRoot = DataRoot();
    const fs::path engineRoot = EngineRoot();
    const fs::path sharedScriptsRoot = SharedScriptsRoot();
    if (fs::exists(sharedScriptsRoot)) {
        m_engine.GetResourceManager().ScanDirectory(sharedScriptsRoot.string());
    }
    if (fs::exists(dataRoot)) {
        m_engine.GetResourceManager().ScanDirectory(dataRoot.string());
    }
    if (fs::exists(engineRoot)) {
        m_engine.GetResourceManager().ScanDirectory(engineRoot.string());
    }
    if (!m_previewDataRoot.empty() && fs::exists(m_previewDataRoot)) {
        m_engine.GetResourceManager().ScanDirectory(m_previewDataRoot.string());
    }

    m_targetTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_width, m_height);
    if (!m_targetTexture) {
        m_status = "Preview texture creation failed";
        m_faulted = true;
        Shutdown();
        m_reloadRequested = false;
        return false;
    }
    SDL_SetTextureBlendMode(m_targetTexture, SDL_BLENDMODE_BLEND);

    std::string script;
    const fs::path sharedPreviewScript = SharedScriptsRoot() / "editor_preview.lua";
    if (fs::exists(sharedPreviewScript)) {
        script = ReadTextFile(sharedPreviewScript);
    }
    if (script.empty()) {
        script = m_engine.GetResourceManager().LoadText("Scripts/editor_preview.lua");
    }
    if (script.empty()) {
        m_status = "Preview driver missing: Scripts/editor_preview.lua";
        m_faulted = true;
        Shutdown();
        m_reloadRequested = false;
        return false;
    }

    m_engine.GetLuaState()["__preview_scene_path"] = m_scenePath;
    m_engine.GetLuaState()["__preview_generated_scene_script"] = m_storyScriptPath;
    m_engine.GetLuaState()["__preview_font_name"] = std::string("NotoSansTC-Bold.ttf");
    m_engine.GetLuaState()["__preview_font_size"] = 32;

    auto scriptResult = m_engine.GetLuaState().safe_script(script, sol::script_pass_on_error);
    if (!scriptResult.valid()) {
        sol::error error = scriptResult;
        m_status = "Preview script error";
        Log("Preview driver failed: " + std::string(error.what()));
        m_faulted = true;
        Shutdown();
        m_reloadRequested = false;
        return false;
    }

    sol::protected_function boot = m_engine.GetLuaState()["EditorPreviewBoot"];
    if (!boot.valid()) {
        m_status = "Preview boot function missing";
        m_faulted = true;
        Shutdown();
        m_reloadRequested = false;
        return false;
    }

    auto bootResult = boot();
    if (!bootResult.valid()) {
        sol::error error = bootResult;
        m_status = "Preview boot failed";
        Log("Preview boot failed: " + std::string(error.what()));
        m_faulted = true;
        Shutdown();
        m_reloadRequested = false;
        return false;
    }

    m_initialized = true;
    m_faulted = false;
    m_reloadRequested = false;
    m_status = "Running " + m_scenePath;
    return true;
}

bool GamePreviewHost::RenderFrame(SDL_Window* window, SDL_Renderer* renderer, int mouseX, int mouseY, bool leftClick, bool rightClick) {
    if (m_faulted && !m_reloadRequested) {
        return false;
    }

    if (m_reloadRequested || !m_initialized) {
        if (!InitializeRuntime(window, renderer)) {
            return false;
        }
    }

    if (!m_targetTexture) {
        m_status = "Preview target unavailable";
        return false;
    }

    m_engine.InjectInputState(mouseX, mouseY, leftClick, rightClick, 0);

    sol::protected_function frame = m_engine.GetLuaState()["EditorPreviewFrame"];
    if (!frame.valid()) {
        m_status = "Preview frame function missing";
        m_faulted = true;
        return false;
    }

    SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, m_targetTexture);
    const auto result = frame();
    SDL_SetRenderTarget(renderer, previousTarget);

    if (!result.valid()) {
        sol::error error = result;
        m_status = "Preview frame failed";
        Log("Preview frame failed: " + std::string(error.what()));
        m_faulted = true;
        return false;
    }

    return true;
}

void GamePreviewHost::Shutdown() {
    if (m_targetTexture) {
        SDL_DestroyTexture(m_targetTexture);
        m_targetTexture = nullptr;
    }

    if (m_engineInitialized) {
        m_engine.Clean();
        m_engineInitialized = false;
    }
    m_initialized = false;
}

fs::path GamePreviewHost::DataRoot() const {
    if (!m_projectRoot.empty() && fs::exists(m_projectRoot / "Data")) {
        return m_projectRoot / "Data";
    }
    return m_projectRoot;
}

fs::path GamePreviewHost::EngineRoot() const {
    if (!m_projectRoot.empty() && fs::exists(m_projectRoot / "Engine")) {
        return m_projectRoot / "Engine";
    }
    return m_workspaceRoot / "Engine";
}

fs::path GamePreviewHost::SharedScriptsRoot() const {
    for (const fs::path& candidate : {m_workspaceRoot / "PrismatiXEngine" / "Scripts", m_workspaceRoot / "Scripts"}) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

void GamePreviewHost::Log(const std::string& message) const {
    if (m_logCallback) {
        m_logCallback(message);
    }
}

}  // namespace PrismatiX::Editor
