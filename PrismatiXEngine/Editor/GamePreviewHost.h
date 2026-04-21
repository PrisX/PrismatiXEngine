#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "Core/Engine.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace PrismatiX::Editor {

class GamePreviewHost {
public:
    using LogCallback = std::function<void(const std::string&)>;

    explicit GamePreviewHost(LogCallback logCallback = {});
    ~GamePreviewHost();

    void SetWorkspaceRoot(const std::filesystem::path& workspaceRoot);
    void SetProjectRoot(const std::filesystem::path& projectRoot);
    void SetPreviewDataRoot(const std::filesystem::path& previewDataRoot);
    void SetScenePath(const std::string& scenePath);
    void SetStoryScriptPath(const std::string& scriptPath);
    void SetViewportSize(int width, int height);
    void RequestReload();
    bool RenderFrame(SDL_Window* window, SDL_Renderer* renderer, int mouseX, int mouseY, bool leftClick, bool rightClick);
    void Shutdown();

    [[nodiscard]] SDL_Texture* GetTexture() const { return m_targetTexture; }
    [[nodiscard]] std::string Status() const { return m_status; }
    [[nodiscard]] std::string ScenePath() const { return m_scenePath; }

private:
    bool InitializeRuntime(SDL_Window* window, SDL_Renderer* renderer);
    void Log(const std::string& message) const;
    [[nodiscard]] std::filesystem::path DataRoot() const;
    [[nodiscard]] std::filesystem::path EngineRoot() const;
    [[nodiscard]] std::filesystem::path SharedScriptsRoot() const;

    LogCallback m_logCallback;
    PrismatiX::App::Engine m_engine;
    std::filesystem::path m_workspaceRoot;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_previewDataRoot;
    SDL_Texture* m_targetTexture = nullptr;
    std::string m_scenePath = "Scripts/scenes/title_scene.lua";
    std::string m_storyScriptPath = "Script/chapter1.pds";
    std::string m_status = "Preview offline";
    int m_width = 1280;
    int m_height = 720;
    bool m_engineInitialized = false;
    bool m_initialized = false;
    bool m_faulted = false;
    bool m_reloadRequested = true;
};

}  // namespace PrismatiX::Editor
