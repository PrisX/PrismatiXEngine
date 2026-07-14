#include "Editor/Preview/RuntimeHost.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Widgets.h"
#include "Engine/Video/VideoPlayer.h"
#include "Engine/Core/TypeRegistry.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace px::editor {
namespace {
Variant PreviewDefault(VariantType type){switch(type){case VariantType::Bool:return Variant(false);case VariantType::Integer:return Variant(std::int64_t{0});case VariantType::Number:return Variant(0.0);case VariantType::String:return Variant(std::string("Preview"));case VariantType::Vec2:return Variant(Vec2{});case VariantType::Rect:return Variant(Rect{});case VariantType::Color:return Variant(Color{});default:return Variant{};}}
}

RuntimeHost::RuntimeHost(SDL_Renderer* renderer) : m_editorRenderer(renderer) {
    m_assets = std::make_unique<graphics::AssetCache>(renderer, m_vfs);
    m_renderer = std::make_unique<graphics::Renderer2D>(renderer, *m_assets);
    m_audio = std::make_unique<audio::AudioEngine>(m_vfs); m_audio->Init();
    m_session = std::make_unique<RuntimeSession>(
        RuntimeSession::Services{m_vfs, *m_audio, *m_renderer, *m_assets});
    m_luaServices.vfs=&m_vfs;m_luaServices.renderer=m_renderer.get();m_luaServices.audio=m_audio.get();m_luaServices.input=&m_input;
    m_luaServices.stage=&m_session->Stage();m_luaServices.variables=&m_session->Variables();
    m_luaServices.routes=&m_session->Routes();m_luaServices.timeline=&m_session->Timeline();
    m_lua=std::make_unique<lua::LuaHost>(m_luaServices);
    (void)m_uiScene.Actions().RegisterProvider(m_lua->CreateActionProvider());
    (void)m_hud.Actions().RegisterProvider(m_lua->CreateActionProvider());
    const auto readVariable=[this](const std::string_view name)->std::optional<Variant>{
        const auto* value=m_session->Variables().GetValue(name);
        return value?std::optional<Variant>(value->Clone()):std::nullopt;
    };
    const auto writeVariable=[this](const std::string_view name,const Variant& value){
        m_session->Variables().SetValue(std::string(name),value.Clone(),vn::VariableScope::SaveLocal);
        return Status::Ok();
    };
    m_uiScene.SetBehaviorVariableAccess(readVariable,writeVariable);
    m_hud.SetBehaviorVariableAccess(readVariable,writeVariable);
    const auto playAnimation=[this](const std::string_view path){return m_session->PlayAnimationAsset(std::string(path));};
    const auto animationPlaying=[this](const std::uint64_t handle){return m_session->Timeline().Playing(handle);};
    m_uiScene.SetExternalAnimationServices(playAnimation,animationPlaying);
    m_hud.SetExternalAnimationServices(playAnimation,animationPlaying);
    const auto configureVideo=[this](ui::Control& control){auto* rectangle=dynamic_cast<ui::VideoRect*>(&control);if(!rectangle)return;auto player=std::make_shared<video::VideoPlayer>(m_renderer->Handle(),m_vfs);rectangle->SetPlayback({[player](const std::string_view path){return player->Open(std::string(path));},[player]{player->Close();},[player](const float delta){player->Update(delta);},[player]{return player->Playing();},[player]{return player->Texture();},[player]{return Vec2{static_cast<float>(player->Width()),static_cast<float>(player->Height())};}});};
    m_uiScene.SetControlRuntimeConfigurator(configureVideo);
    m_hud.SetControlRuntimeConfigurator(configureVideo);
    m_session->SetBehaviorStateHandler(
        [this] { return m_mode==Mode::UIScene ? m_uiScene.CaptureBehaviorState()
                                              : m_hud.CaptureBehaviorState(); },
        [this](const ui::BehaviorRuntimeState& state) {
            return m_mode==Mode::UIScene ? m_uiScene.RestoreBehaviorState(state)
                                         : m_hud.RestoreBehaviorState(state);
        });
    m_session->SetExtensionCommandHandler([this](const vn::Command& command){const bool handled=m_lua&&m_lua->InvokeCommand(command);if(handled&&m_lua->HasPendingCommand())m_session->VM().WaitExternal();return handled;});
    m_hud.SetActionSink([this](const ui::GalgameAction& action) {
        if (action.command == "choice.select") {
            m_session->SelectChoice(std::atoi(action.argument.c_str()));
        }
    });
}

RuntimeHost::~RuntimeHost() { if (m_target) SDL_DestroyTexture(m_target); }
void RuntimeHost::SetProjectRoot(const std::string& root) {
    m_vfs.Clear(); m_vfs.MountDirectory(root); m_assets->Clear(); m_routeScenes.clear();
    m_lua=std::make_unique<lua::LuaHost>(m_luaServices);
    (void)m_uiScene.Actions().UnregisterProvider("lua");
    (void)m_uiScene.Actions().RegisterProvider(m_lua->CreateActionProvider());
    (void)m_hud.Actions().UnregisterProvider("lua");
    (void)m_hud.Actions().RegisterProvider(m_lua->CreateActionProvider());
    m_session->SetExtensionCommandHandler([this](const vn::Command& command){const bool handled=m_lua&&m_lua->InvokeCommand(command);if(handled&&m_lua->HasPendingCommand())m_session->VM().WaitExternal();return handled;});
    if(m_vfs.Exists("Content/Extensions/extensions.pxindex"))(void)m_lua->LoadExtensionIndex("Content/Extensions/extensions.pxindex");
    if (const auto manifestText = m_vfs.ReadText("project.pxproject")) {
        const auto manifest = resource::ParseTypedDocument(*manifestText, "project.pxproject");
        if (manifest) {
            const auto routes = manifest.Value().properties.find("routes");
            if (routes != manifest.Value().properties.end())
                if (const auto* values = routes->second.AsArray())
                    for (const auto& value : *values) {
                        const auto* object = value.AsObject(); if (!object) continue;
                        const auto id = object->find("id"), scene = object->find("scene");
                        const auto* route = id != object->end() ? id->second.TryGet<std::string>() : nullptr;
                        const auto* reference = scene != object->end() ? scene->second.TryGet<ResourceRefValue>() : nullptr;
                        if (!route || !reference || reference->lastKnownPath.empty()) continue;
                        m_routeScenes[*route] = reference->lastKnownPath;
                        (void)m_session->Routes().Register(*route, [route = *route]() {
                            return Result<std::unique_ptr<ui::Control>>::Success(
                                std::make_unique<ui::Control>("PreviewRoute:" + route));
                        });
                    }
        }
    }
    m_session->SetRoutePresentationHandler([this](std::string_view route, std::string_view operation) {
        if (operation == "back") { if (!m_vnScript.empty()) { m_mode = Mode::Vn; m_hud.ShowHUD(DialogueUI()); } return; }
        const auto found = m_routeScenes.find(std::string(route));
        if (found != m_routeScenes.end()) LoadUI(found->second);
    });
}

void RuntimeHost::EnsureTarget() {
    if (m_target) return;
    m_target = SDL_CreateTexture(m_editorRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_width*m_targetDensity, m_height*m_targetDensity);
    if (m_target) SDL_SetTextureScaleMode(m_target, SDL_SCALEMODE_LINEAR);
}

void RuntimeHost::SetDisplayScale(float scale,bool pixelExact){
    m_displayScale=std::max(.01f,scale);m_pixelExactPreview=pixelExact;
    const float minimum=pixelExact?1.0f:2.0f;
    const int density=std::clamp(static_cast<int>(std::ceil(std::max(minimum,m_displayScale))),pixelExact?1:2,4);
    if(density==m_targetDensity)return;m_targetDensity=density;if(m_target){SDL_DestroyTexture(m_target);m_target=nullptr;}
}

std::optional<Vec2> RuntimeHost::ImageSize(const std::string& path){
    SDL_Texture* texture=m_assets?m_assets->Texture(path):nullptr;if(!texture)return std::nullopt;
    int width=0,height=0;graphics::AssetCache::TextureSize(texture,width,height);
    if(width<=0||height<=0)return std::nullopt;return Vec2{static_cast<float>(width),static_cast<float>(height)};
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
    for(const auto& node:document.nodes){const auto bindings=node.properties.find("bindings");if(bindings==node.properties.end())continue;const auto* definitions=bindings->second.AsObject();if(!definitions)continue;for(const auto& [targetName,value]:*definitions){const auto* definition=value.AsObject();if(!definition)continue;const auto pathValue=definition->find("path");const auto* bindingPath=pathValue!=definition->end()?pathValue->second.TryGet<std::string>():nullptr;if(!bindingPath||m_previewViewModel.Describe(*bindingPath))continue;const auto* property=TypeRegistry::Global().FindProperty(node.type,targetName);if(!property)continue;VariantType sourceType=property->type;if(const auto format=definition->find("formatter");format!=definition->end())if(const auto* name=format->second.TryGet<std::string>())if(const auto* formatter=m_uiScene.Formatters().Find(*name))sourceType=formatter->input;m_previewViewModel.Define(*bindingPath,PreviewDefault(sourceType),true);}}
    const ui::UIDocumentLoader loader=[this](const ResourceRefValue& reference)->Result<resource::TypedDocument>{
        const auto text=m_vfs.ReadText(reference.lastKnownPath);
        if(!text)return Result<resource::TypedDocument>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDPREV4002",.category="Editor.Preview",.message="Referenced UI resource not found: "+reference.lastKnownPath});
        return resource::ParseTypedDocument(*text,reference.lastKnownPath);
    };
    auto loaded=ui::InstantiateUIScene(document,&m_previewViewModel,m_uiScene.Formatters(),loader); if(!loaded)return;
    auto animations=std::move(loaded.Value().animations);auto theme=std::move(loaded.Value().theme);auto triggers=std::move(loaded.Value().triggers);auto interaction=std::move(loaded.Value().interactionGraph);m_uiBindings=std::move(loaded.Value().bindings); const Status status=m_uiScene.SetRoot(std::move(loaded.Value().root));
    if(!status)for(const auto& d:status.Diagnostics())diag::Emit(d);
    if(theme)m_uiScene.SetTheme(std::move(*theme));
    if(animations){const Status animationStatus=m_uiScene.SetAnimations(std::move(*animations),true);if(!animationStatus)for(const auto& d:animationStatus.Diagnostics())diag::Emit(d);}
    {const Status triggerStatus=m_uiScene.ConfigureTriggers(std::move(triggers),std::move(interaction),sourcePath);if(!triggerStatus)for(const auto& d:triggerStatus.Diagnostics())diag::Emit(d);}
    m_lastUIDocument=document;
}

bool RuntimeHost::ApplyUIDocumentPatch(const resource::TypedDocument& document,const std::string& sourcePath){
    if(!m_lastUIDocument||!m_uiScene.Root()||m_lastUIDocument->nodes.size()!=document.nodes.size()||
       m_lastUIDocument->properties!=document.properties){LoadUIDocument(document,sourcePath);return false;}
    for(std::size_t index=0;index<document.nodes.size();++index){const auto& before=m_lastUIDocument->nodes[index];const auto& after=document.nodes[index];
        if(before.id!=after.id||before.parent!=after.parent||before.type!=after.type){LoadUIDocument(document,sourcePath);return false;}
        if(before.properties.contains("bindings")!=after.properties.contains("bindings")||before.properties.contains("triggers")!=after.properties.contains("triggers")||
           (before.properties.contains("bindings")&&before.properties.at("bindings")!=after.properties.at("bindings"))||
           (before.properties.contains("triggers")&&before.properties.at("triggers")!=after.properties.at("triggers"))){LoadUIDocument(document,sourcePath);return false;}}
    for(std::size_t index=0;index<document.nodes.size();++index){const auto& before=m_lastUIDocument->nodes[index];const auto& after=document.nodes[index];
        auto* object=m_uiScene.Root()->Find(after.id);if(!object){LoadUIDocument(document,sourcePath);return false;}if(before.name!=after.name)object->SetName(after.name);
        for(const auto& [name,value]:after.properties){const auto prior=before.properties.find(name);if(prior!=before.properties.end()&&prior->second==value)continue;
            if(name=="bindings"||name=="triggers"||name=="styleBinding")continue;const auto* property=TypeRegistry::Global().FindProperty(after.type,name);
            if(property&&property->set){const Status status=property->set(*object,value);if(!status){LoadUIDocument(document,sourcePath);return false;}}}
    }
    m_uiPath=sourcePath;m_mode=Mode::UIScene;m_lastUIDocument=document;return true;
}

Status RuntimeHost::PreviewUIAnimation(const Uuid& clip,float time,bool playing){
    if(m_mode!=Mode::UIScene)return Status::Ok();
    return m_uiScene.PreviewAnimation(clip,time,playing);
}

Status RuntimeHost::SetUIAnimationParameter(const std::string_view parameter,const Variant& value){
    if(m_mode!=Mode::UIScene)return Status::Ok();
    return m_uiScene.SetAnimationParameter(parameter,value);
}

void RuntimeHost::LoadVn(const std::string& script) {
    m_vnScript=script;m_mode=Mode::Vn;m_session->StartScenario(script);m_hud.ShowHUD(DialogueUI());
}
void RuntimeHost::Reload(){if(m_mode==Mode::UIScene)LoadUI(m_uiPath);else LoadVn(m_vnScript);}

ui::DialoguePresentation RuntimeHost::DialogueUI() const {
    const auto& dialogue=m_session->Dialogue().State();ui::DialoguePresentation view;view.speaker=dialogue.speaker;view.text=dialogue.displayText;view.chapterTitle=m_session->VM().Chapter();view.musicTitle=m_session->VM().CurrentBgm();view.choices=m_choiceTexts;view.effect=dialogue.effect;view.effectProgress=dialogue.effectProgress;return view;
}

void RuntimeHost::Tick(float dt,std::uint64_t nowMs,bool hovered,float x,float y,bool click){
    EnsureTarget();if(!m_target)return;m_assets->BeginFrame();m_audio->Update();m_input.InjectFrame(hovered?x:-1000,hovered?y:-1000,hovered&&click);
    SDL_Texture* previous=SDL_GetRenderTarget(m_editorRenderer);SDL_SetRenderTarget(m_editorRenderer,m_target);SDL_SetRenderLogicalPresentation(m_editorRenderer,0,0,SDL_LOGICAL_PRESENTATION_DISABLED);SDL_SetRenderScale(m_editorRenderer,1,1);
    SDL_SetRenderDrawColor(m_editorRenderer,12,14,20,255);SDL_RenderClear(m_editorRenderer);
    m_renderer->SetLogicalSize(m_width,m_height,false);SDL_SetRenderLogicalPresentation(m_editorRenderer,0,0,SDL_LOGICAL_PRESENTATION_DISABLED);SDL_SetRenderScale(m_editorRenderer,static_cast<float>(m_targetDensity),static_cast<float>(m_targetDensity));m_renderer->SetPreviewContext(m_displayScale,m_mode==Mode::UIScene&&!m_pixelExactPreview);
    if(m_lua){const bool pending=m_lua->HasPendingCommand();m_lua->Update(dt);if(pending&&!m_lua->HasPendingCommand())m_session->VM().NotifyExternalDone();m_lua->Emit("frame.update",{{"delta",std::to_string(dt)}});}
    if(m_mode==Mode::UIScene){m_session->Timeline().Update(dt);(void)m_uiScene.Update(m_input,m_width,m_height,dt);m_uiScene.Render(*m_renderer);}else{
        auto& vm=m_session->VM();m_choiceTexts.clear();if(vm.State()==vn::VMState::WaitingChoice)for(const auto& c:vm.Choices())m_choiceTexts.push_back(c.text);
        m_hud.RefreshHUD(DialogueUI());const bool consumed=m_hud.Update(m_input,m_width,m_height,dt);
        if(hovered&&click&&!consumed&&vm.State()!=vn::VMState::WaitingChoice)m_session->Advance();
        m_session->Update(nowMs,dt);m_session->Stage().Render();m_hud.RefreshHUD(DialogueUI());m_hud.Render(*m_renderer);
    }
    SDL_SetRenderScale(m_editorRenderer,1,1);SDL_SetRenderTarget(m_editorRenderer,previous);
}

}  // namespace px::editor
