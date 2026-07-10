#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/UIContext.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::editor {

class RuntimeHost {
public:
    enum class Mode { UIScene, Vn };
    explicit RuntimeHost(SDL_Renderer* editorRenderer);
    ~RuntimeHost();

    void SetProjectRoot(const std::string& root);
    void SetMode(Mode mode) { m_mode = mode; }
    [[nodiscard]] Mode GetMode() const { return m_mode; }
    void LoadUI(const std::string& vfsPath);
    void LoadUIDocument(const resource::TypedDocument& document, const std::string& sourcePath);
    void LoadVn(const std::string& script);
    void Reload();
    void Tick(float dt, std::uint64_t nowMs, bool hovered, float localX, float localY, bool click);

    [[nodiscard]] SDL_Texture* Target() const { return m_target; }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }
    [[nodiscard]] const std::string& CurrentUIPath() const { return m_uiPath; }
    [[nodiscard]] const vn::VM& VMRef() const { return *m_vm; }
    [[nodiscard]] vn::VM& VMRef() { return *m_vm; }
    [[nodiscard]] audio::AudioEngine& AudioRef() { return *m_audio; }
    [[nodiscard]] const vn::VariableStore& Vars() const { return m_vars; }
    [[nodiscard]] const vn::Dialogue& DialogueRef() const { return m_dialogue; }

private:
    void EnsureTarget();
    [[nodiscard]] ui::DialoguePresentation DialogueUI() const;

    SDL_Renderer* m_editorRenderer;
    SDL_Texture* m_target = nullptr;
    int m_width = 1280, m_height = 720;
    Mode m_mode = Mode::UIScene;
    io::VFS m_vfs;
    std::unique_ptr<graphics::AssetCache> m_assets;
    std::unique_ptr<graphics::Renderer2D> m_renderer;
    std::unique_ptr<audio::AudioEngine> m_audio;
    Input m_input;
    std::unique_ptr<vn::Stage> m_stage;
    vn::Dialogue m_dialogue;
    vn::VariableStore m_vars;
    vn::Backlog m_backlog;
    std::unique_ptr<vn::VM> m_vm;
    ui::UIContext m_uiScene;
    ui::ObservableViewModel m_previewViewModel;
    std::vector<ui::Binding> m_uiBindings;
    ui::GalgameUI m_hud;
    std::vector<std::string> m_choiceTexts;
    std::string m_uiPath;
    std::string m_vnScript;
};

}  // namespace px::editor
