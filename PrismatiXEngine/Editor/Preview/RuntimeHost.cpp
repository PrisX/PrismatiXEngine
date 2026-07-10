#include "Editor/Preview/RuntimeHost.h"

#include "Engine/UI/UISchema.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

namespace px::editor {

RuntimeHost::RuntimeHost(SDL_Renderer* editorRenderer) : m_editorRenderer(editorRenderer) {
    m_assets = std::make_unique<graphics::AssetCache>(editorRenderer, m_vfs);
    m_renderer = std::make_unique<graphics::Renderer2D>(editorRenderer, *m_assets);
    m_audio = std::make_unique<audio::AudioEngine>(m_vfs);
    m_audio->Init();  // preview VN audio + asset-browser audition
    m_stage = std::make_unique<vn::Stage>(*m_renderer, *m_assets);
    m_vm = std::make_unique<vn::VM>(m_vfs, *m_audio, *m_stage, m_dialogue, m_vars, m_backlog);
    m_hud.SetDialogue(&m_dialogue.State());
    m_hud.SetChoices(&m_choiceTexts);
}

RuntimeHost::~RuntimeHost() {
    if (m_target) {
        SDL_DestroyTexture(m_target);
    }
}

void RuntimeHost::SetProjectRoot(const std::string& root) {
    m_vfs.Clear();
    m_vfs.MountDirectory(root);
    m_assets->Clear();
}

void RuntimeHost::EnsureTarget() {
    if (m_target) {
        return;
    }
    m_target = SDL_CreateTexture(m_editorRenderer, SDL_PIXELFORMAT_RGBA8888,
                                 SDL_TEXTUREACCESS_TARGET, m_width, m_height);
    if (m_target) {
        SDL_SetTextureScaleMode(m_target, SDL_SCALEMODE_LINEAR);
    }
}

void RuntimeHost::LoadUI(const std::string& vfsPath) {
    m_uiPath = vfsPath;
    m_mode = Mode::UIScene;
    auto text = m_vfs.ReadText(vfsPath);
    if (!text) {
        PX_LOG_WARN("RuntimeHost: UI not found '{}'", vfsPath);
        return;
    }
    if (auto scene = px::ui::ParsePXUI(*text)) {
        m_uiStage.Load(std::move(*scene));
    }
}

void RuntimeHost::LoadVn(const std::string& script) {
    m_vnScript = script;
    m_mode = Mode::Vn;
    m_stage->ClearAll();
    m_vars.Reset(false);
    m_backlog.Clear();
    if (auto text = m_vfs.ReadText("Data/UI/hud.pxui")) {
        if (auto scene = px::ui::ParsePXUI(*text)) {
            m_hud.Load(std::move(*scene));
        }
    }
    m_vm->LoadScript(script);
}

void RuntimeHost::Reload() {
    if (m_mode == Mode::UIScene) {
        LoadUI(m_uiPath);
    } else {
        LoadVn(m_vnScript);
    }
}

void RuntimeHost::Tick(float dt, std::uint64_t nowMs, bool hovered, float localX, float localY,
                       bool click) {
    EnsureTarget();
    if (!m_target) {
        return;
    }

    m_assets->BeginFrame();
    m_audio->Update();
    m_input.InjectFrame(hovered ? localX : -1000.0f, hovered ? localY : -1000.0f,
                        hovered && click);

    SDL_Texture* prevTarget = SDL_GetRenderTarget(m_editorRenderer);
    SDL_SetRenderTarget(m_editorRenderer, m_target);
    SDL_SetRenderScale(m_editorRenderer, 1.0f, 1.0f);
    m_renderer->SetLogicalSize(m_width, m_height);
    SDL_SetRenderDrawColor(m_editorRenderer, 12, 14, 20, 255);
    SDL_RenderClear(m_editorRenderer);

    if (m_mode == Mode::UIScene) {
        m_uiStage.Update(m_input, dt);
        m_uiStage.Render(*m_renderer);
    } else {
        m_choiceTexts.clear();
        if (m_vm->State() == vn::VMState::WaitingChoice) {
            for (const auto& c : m_vm->Choices()) m_choiceTexts.push_back(c.text);
        }
        if (auto act = m_hud.Update(m_input, dt)) {
            if (act->type == "choice") m_vm->SelectChoice(std::atoi(act->arg.c_str()));
        } else if (hovered && click && m_vm->State() != vn::VMState::WaitingChoice) {
            m_vm->OnAdvance();
        }
        m_vm->Update(nowMs, dt);
        m_stage->Render();
        m_hud.Render(*m_renderer);
    }

    SDL_SetRenderTarget(m_editorRenderer, prevTarget);
}

}
