#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Gfx/AssetCache.h"
#include "Engine/Gfx/Renderer2D.h"
#include "Engine/IO/Vfs.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/UiStage.h"
#include "Engine/VN/Backlog.h"
#include "Engine/VN/Dialogue.h"
#include "Engine/VN/Stage.h"
#include "Engine/VN/VariableStore.h"
#include "Engine/VN/Vm.h"

#include <memory>
#include <string>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::editor {

class RuntimeHost {
public:
    enum class Mode { UiScene, Vn };

    explicit RuntimeHost(SDL_Renderer* editorRenderer);
    ~RuntimeHost();

    void SetProjectRoot(const std::string& root);
    void SetMode(Mode mode) { m_mode = mode; }
    [[nodiscard]] Mode GetMode() const { return m_mode; }

    void LoadUi(const std::string& vfsPath);
    void LoadVn(const std::string& script);
    void Reload();

    void Tick(float dt, std::uint64_t nowMs, bool hovered, float localX, float localY, bool click);

    [[nodiscard]] SDL_Texture* Target() const { return m_target; }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }
    [[nodiscard]] const px::ui::UiStage& UiStageRef() const { return m_uiStage; }
    [[nodiscard]] px::ui::UiStage& UiStageRef() { return m_uiStage; }
    [[nodiscard]] const std::string& CurrentUiPath() const { return m_uiPath; }

private:
    void EnsureTarget();

    SDL_Renderer* m_editorRenderer;
    SDL_Texture* m_target = nullptr;
    int m_width = 1280;
    int m_height = 720;
    Mode m_mode = Mode::UiScene;

    io::Vfs m_vfs;
    std::unique_ptr<gfx::AssetCache> m_assets;
    std::unique_ptr<gfx::Renderer2D> m_renderer;
    std::unique_ptr<audio::AudioEngine> m_audio;
    Input m_input;

    std::unique_ptr<vn::Stage> m_stage;
    vn::Dialogue m_dialogue;
    vn::VariableStore m_vars;
    vn::Backlog m_backlog;
    std::unique_ptr<vn::Vm> m_vm;

    px::ui::UiStage m_uiStage;
    px::ui::UiStage m_hud;
    std::vector<std::string> m_choiceTexts;

    std::string m_uiPath;
    std::string m_vnScript;
};

}
