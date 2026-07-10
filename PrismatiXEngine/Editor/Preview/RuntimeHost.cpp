#include "Editor/Preview/RuntimeHost.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/Core/TypeRegistry.h"

#include <SDL3/SDL.h>

namespace px::editor {
namespace {
Variant PreviewDefault(VariantType type){switch(type){case VariantType::Bool:return Variant(false);case VariantType::Integer:return Variant(std::int64_t{0});case VariantType::Number:return Variant(0.0);case VariantType::String:return Variant(std::string("Preview"));case VariantType::Vec2:return Variant(Vec2{});case VariantType::Rect:return Variant(Rect{});case VariantType::Color:return Variant(Color{});default:return Variant{};}}
}

RuntimeHost::RuntimeHost(SDL_Renderer* renderer) : m_editorRenderer(renderer) {
    m_assets = std::make_unique<graphics::AssetCache>(renderer, m_vfs);
    m_renderer = std::make_unique<graphics::Renderer2D>(renderer, *m_assets);
    m_audio = std::make_unique<audio::AudioEngine>(m_vfs); m_audio->Init();
    m_stage = std::make_unique<vn::Stage>(*m_renderer, *m_assets);
    m_vm = std::make_unique<vn::VM>(m_vfs, *m_audio, *m_stage, m_dialogue, m_vars, m_backlog);
    m_hud.SetActionSink([this](const ui::GalgameAction& action) {
        if (action.command == "choice.select") m_vm->SelectChoice(std::atoi(action.argument.c_str()));
    });
}

RuntimeHost::~RuntimeHost() { if (m_target) SDL_DestroyTexture(m_target); }
void RuntimeHost::SetProjectRoot(const std::string& root) { m_vfs.Clear(); m_vfs.MountDirectory(root); m_assets->Clear(); }

void RuntimeHost::EnsureTarget() {
    if (m_target) return;
    m_target = SDL_CreateTexture(m_editorRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_width, m_height);
    if (m_target) SDL_SetTextureScaleMode(m_target, SDL_SCALEMODE_LINEAR);
}

void RuntimeHost::LoadUI(const std::string& path) {
    auto text = m_vfs.ReadText(path);
    if (!text) {
        diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXEDPREV4001",.category="Editor.Preview",.message="Typed UI scene not found: "+path};
        d.source.path=path;diag::Emit(d);return;
    }
    auto document=resource::ParseTypedDocument(*text,path); if(!document){for(const auto& d:document.Diagnostics())diag::Emit(d);return;}
    LoadUIDocument(document.Value(), path);
}

void RuntimeHost::LoadUIDocument(const resource::TypedDocument& document,
                                 const std::string& sourcePath) {
    m_uiPath = sourcePath; m_mode = Mode::UIScene; m_uiBindings.clear();
    ui::RegisterBuiltinUITypes();m_previewViewModel=ui::ObservableViewModel{};
    for(const auto& node:document.nodes)for(const auto& [key,value]:node.properties)if(key.starts_with("bind."))if(const auto* bindingPath=value.TryGet<std::string>()){
        const auto* property=TypeRegistry::Global().FindProperty(node.type,key.substr(5));if(!property||m_previewViewModel.Describe(*bindingPath))continue;
        VariantType sourceType=property->type;if(const auto format=node.properties.find("formatter."+key.substr(5));format!=node.properties.end())if(const auto* name=format->second.TryGet<std::string>())if(const auto* formatter=m_uiScene.Formatters().Find(*name))sourceType=formatter->input;
        m_previewViewModel.Define(*bindingPath,PreviewDefault(sourceType),true);
    }
    auto loaded=ui::InstantiateUIScene(document,&m_previewViewModel,m_uiScene.Formatters()); if(!loaded)return;
    auto animation=std::move(loaded.Value().animation);auto theme=std::move(loaded.Value().theme);m_uiBindings=std::move(loaded.Value().bindings); const Status status=m_uiScene.SetRoot(std::move(loaded.Value().root));
    if(!status)for(const auto& d:status.Diagnostics())diag::Emit(d);
    if(theme)m_uiScene.SetTheme(std::move(*theme));
    if(animation){const Status animationStatus=m_uiScene.SetAnimation(std::move(*animation),true);if(!animationStatus)for(const auto& d:animationStatus.Diagnostics())diag::Emit(d);}
}

void RuntimeHost::LoadVn(const std::string& script) {
    m_vnScript=script;m_mode=Mode::Vn;m_stage->ClearAll();m_vars.Reset(false);m_backlog.Clear();m_vm->LoadScript(script);m_hud.ShowHUD(DialogueUI());
}
void RuntimeHost::Reload(){if(m_mode==Mode::UIScene)LoadUI(m_uiPath);else LoadVn(m_vnScript);}

ui::DialoguePresentation RuntimeHost::DialogueUI() const {
    ui::DialoguePresentation view;view.speaker=m_dialogue.State().speaker;view.text=m_dialogue.State().displayText;view.choices=m_choiceTexts;return view;
}

void RuntimeHost::Tick(float dt,std::uint64_t nowMs,bool hovered,float x,float y,bool click){
    EnsureTarget();if(!m_target)return;m_assets->BeginFrame();m_audio->Update();m_input.InjectFrame(hovered?x:-1000,hovered?y:-1000,hovered&&click);
    SDL_Texture* previous=SDL_GetRenderTarget(m_editorRenderer);SDL_SetRenderTarget(m_editorRenderer,m_target);SDL_SetRenderScale(m_editorRenderer,1,1);
    m_renderer->SetLogicalSize(m_width,m_height);SDL_SetRenderDrawColor(m_editorRenderer,12,14,20,255);SDL_RenderClear(m_editorRenderer);
    if(m_mode==Mode::UIScene){(void)m_uiScene.Update(m_input,m_width,m_height,dt);m_uiScene.Render(*m_renderer);}else{
        m_choiceTexts.clear();if(m_vm->State()==vn::VMState::WaitingChoice)for(const auto& c:m_vm->Choices())m_choiceTexts.push_back(c.text);
        m_hud.RefreshHUD(DialogueUI());const bool consumed=m_hud.Update(m_input,m_width,m_height,dt);
        if(hovered&&click&&!consumed&&m_vm->State()!=vn::VMState::WaitingChoice)m_vm->OnAdvance();
        m_vm->Update(nowMs,dt);m_stage->Render();m_hud.RefreshHUD(DialogueUI());m_hud.Render(*m_renderer);
    }
    SDL_SetRenderTarget(m_editorRenderer,previous);
}

}  // namespace px::editor
