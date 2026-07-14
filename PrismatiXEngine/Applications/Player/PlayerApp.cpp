#include "Applications/Player/PlayerApp.h"

#include "Engine/Graphics/Screenshot.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/Widgets.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <optional>
#include <sstream>
#include <utility>

namespace px::player {

namespace {
const std::string kSaveKey = "prismatix-demo-secret";
constexpr int kAutoSaveSlot = -1;

int ScancodeFromName(const std::string& name) {
    if (name == "Escape") return SDL_SCANCODE_ESCAPE;
    if (name == "Tab") return SDL_SCANCODE_TAB;
    if (name == "Space") return SDL_SCANCODE_SPACE;
    if (name == "Enter" || name == "Return") return SDL_SCANCODE_RETURN;
    if (name == "Backspace") return SDL_SCANCODE_BACKSPACE;
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        return SDL_SCANCODE_A + (name[0] - 'A');
    }
    if (name.size() >= 2 && name[0] == 'F') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) return SDL_SCANCODE_F1 + (n - 1);
    }
    return SDL_SCANCODE_UNKNOWN;
}

std::optional<resource::TypedDocument> LoadTypedFile(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);if(!stream)return std::nullopt;std::ostringstream text;text<<stream.rdbuf();
    auto parsed=resource::ParseTypedDocument(text.str(),path.string());if(!parsed){for(const auto& d:parsed.Diagnostics())diag::Emit(d);return std::nullopt;}return std::move(parsed.Value());
}
std::string DocText(const resource::TypedDocument& document,const char* key,std::string fallback={}){const auto it=document.properties.find(key);if(it!=document.properties.end())if(const auto* value=it->second.TryGet<std::string>())return *value;return fallback;}
int DocInt(const resource::TypedDocument& document,const char* key,int fallback){const auto it=document.properties.find(key);if(it!=document.properties.end())if(const auto* value=it->second.TryGet<std::int64_t>())return static_cast<int>(*value);return fallback;}
bool DocBool(const resource::TypedDocument& document,const char* key,bool fallback){const auto it=document.properties.find(key);if(it!=document.properties.end())if(const auto* value=it->second.TryGet<bool>())return *value;return fallback;}
std::vector<std::string> DocArchives(const resource::TypedDocument& document){std::vector<std::string> result;const auto found=document.properties.find("archives");if(found==document.properties.end()||!found->second.AsArray())return result;for(const auto& value:*found->second.AsArray()){const auto* object=value.AsObject();if(!object)return {};const auto file=object->find("file"),group=object->find("group"),optional=object->find("optional");const auto* path=file!=object->end()?file->second.TryGet<std::string>():nullptr;const auto* id=group!=object->end()?group->second.TryGet<std::string>():nullptr;const auto* optionalValue=optional!=object->end()?optional->second.TryGet<bool>():nullptr;if(!path||path->empty()||!id||id->empty()||!optionalValue)return {};result.push_back(*path);}return result;}
std::unordered_map<std::string,std::string> DocRoutes(const resource::TypedDocument& document){std::unordered_map<std::string,std::string> result;const auto found=document.properties.find("routes");if(found==document.properties.end()||!found->second.AsArray())return result;for(const auto& value:*found->second.AsArray()){const auto* object=value.AsObject();if(!object)continue;const auto id=object->find("id"),scene=object->find("scene");const auto* routeId=id!=object->end()?id->second.TryGet<std::string>():nullptr;const auto* reference=scene!=object->end()?scene->second.TryGet<ResourceRefValue>():nullptr;if(routeId&&reference&&!reference->id.Empty()&&!reference->lastKnownPath.empty())result[*routeId]=reference->lastKnownPath;}return result;}
}

PlayerApp::Boot PlayerApp::LoadBootConfig() {
    Boot boot;
    boot.config.title = "PrismatiX Player";
    boot.config.mountDirs = { "." };

    if (auto package=LoadTypedFile("game.pxpackage")) {
        if(package->kind==resource::DocumentKind::Resource&&package->type=="GamePackage"){
            boot.packaged = true;
            boot.config.title = DocText(*package,"title",boot.config.title);
            boot.config.mountDirs.clear();
            boot.config.mountArchives = DocArchives(*package);
            if(boot.config.mountArchives.empty()){diag::Emit(diag::Diagnostic{.severity=diag::Severity::Fatal,.code="PXPLAYER5005",.category="Player.Boot",.message="Package archive list is missing or invalid"});return boot;}
            if(DocBool(*package,"encrypt",true))boot.config.archiveKey=DocText(*package,"key");
            boot.config.width=boot.config.logicalWidth=DocInt(*package,"gameWidth",1280);
            boot.config.height=boot.config.logicalHeight=DocInt(*package,"gameHeight",720);
            boot.startScript=DocText(*package,"startScript",boot.startScript);
            boot.startRoute=DocText(*package,"startRoute",boot.startRoute);boot.routeScenes=DocRoutes(*package);
            boot.saveSecret=DocText(*package,"key");
            if (boot.saveSecret.empty()) boot.saveSecret = boot.config.title;
            boot.valid = true;
            PX_LOG_INFO("Packaged build: mounting {} content group(s)", boot.config.mountArchives.size());
            return boot;
        }
    }

    if(auto project=LoadTypedFile("project.pxproject")){
        if(project->kind==resource::DocumentKind::Project&&project->type=="PrismatiXProject"){
            boot.config.title=DocText(*project,"name",boot.config.title);
            boot.config.width=boot.config.logicalWidth=DocInt(*project,"gameWidth",1280);
            boot.config.height=boot.config.logicalHeight=DocInt(*project,"gameHeight",720);
            boot.startScript=DocText(*project,"startScript",boot.startScript);
            boot.startRoute=DocText(*project,"startRoute",boot.startRoute);boot.routeScenes=DocRoutes(*project);
            boot.saveSecret=DocText(*project,"encryptKey",boot.config.title);
            PX_LOG_INFO("Dev run: project '{}', entry '{}', route '{}'", boot.config.title,
                        boot.startScript, boot.startRoute);
            boot.valid = true;
        }
    } else {
        diag::Diagnostic d{.severity=diag::Severity::Fatal,.code="PXPLAYER5002",.category="Player.Boot",.message="No game.pxpackage or project.pxproject was found."};diag::Emit(d);
    }
    return boot;
}

bool PlayerApp::Init(int argc, char* argv[]) {
    m_boot = LoadBootConfig();
    if (!m_boot.valid) return false;
    if (!m_runtime.Init(m_boot.config)) {
        PX_LOG_CRITICAL("Failed to initialize runtime.");
        return false;
    }

    // Save files are keyed per project; the fixed default only covers projects
    // that ship neither a key nor a title.
    m_saveKey = crypto::DeriveKey(
        m_boot.saveSecret.empty() ? std::string(kSaveKey) : m_boot.saveSecret + "|px-save");
    m_profile.Load("Save/profile.dat", &m_saveKey);
    m_settings.Load("Save/config.dat", &m_saveKey);
    m_saves.Configure("Save", &m_saveKey);
    PX_LOG_DEBUG("Player boot: persistence configured");
    m_runtime.Audio().SetBGMVolume(m_settings.bgmVolume);
    m_runtime.Audio().SetSEVolume(m_settings.seVolume);
    m_runtime.Audio().SetVoiceVolume(m_settings.voiceVolume);
    if (m_settings.fullscreen) {
        SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), true);
    } else if (m_settings.windowWidth >= 320 && m_settings.windowHeight >= 180) {
        SDL_SetWindowSize(m_runtime.GetWindow().Handle(), m_settings.windowWidth,
                          m_settings.windowHeight);
        SDL_SetWindowPosition(m_runtime.GetWindow().Handle(), SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED);
    }

    m_session = std::make_unique<RuntimeSession>(RuntimeSession::Services{
        m_runtime.VFS(), m_runtime.Audio(), m_runtime.Renderer(), m_runtime.Assets()});
    m_ui.SetBehaviorVariableAccess(
        [this](const std::string_view name)->std::optional<Variant>{
            const auto* value=m_session->Variables().GetValue(name);
            return value?std::optional<Variant>(value->Clone()):std::nullopt;
        },
        [this](const std::string_view name,const Variant& value){
            m_session->Variables().SetValue(std::string(name),value.Clone(),
                                            vn::VariableScope::SaveLocal);
            return Status::Ok();
        });
    m_ui.SetExternalAnimationServices(
        [this](const std::string_view path){return m_session->PlayAnimationAsset(std::string(path));},
        [this](const std::uint64_t handle){return m_session->Timeline().Playing(handle);});
    m_ui.SetControlRuntimeConfigurator([this](ui::Control& control){auto* rectangle=dynamic_cast<ui::VideoRect*>(&control);if(!rectangle)return;auto player=std::make_shared<video::VideoPlayer>(m_runtime.Renderer().Handle(),m_runtime.VFS());rectangle->SetPlayback({[player](const std::string_view path){return player->Open(std::string(path));},[player]{player->Close();},[player](const float delta){player->Update(delta);},[player]{return player->Playing();},[player]{return player->Texture();},[player]{return Vec2{static_cast<float>(player->Width()),static_cast<float>(player->Height())};}});});
    m_session->SetBehaviorStateHandler(
        [this] { return m_ui.CaptureBehaviorState(); },
        [this](const ui::BehaviorRuntimeState& state) { return m_ui.RestoreBehaviorState(state); });
    if(m_boot.routeScenes.empty()||!m_boot.routeScenes.contains(m_boot.startRoute)){diag::Diagnostic diagnostic{.severity=diag::Severity::Fatal,.code="PXPLAYER5004",.category="Player.Boot",.message="Route table or startRoute is invalid"};diag::Emit(diagnostic);return false;}
    for(const auto& [route,_]:m_boot.routeScenes){
        const Status registered=m_session->Routes().Register(route,[route](){return Result<std::unique_ptr<ui::Control>>::Success(std::make_unique<ui::Control>(std::string("Route:")+route));});
        if(!registered)return false;
    }
    if(!m_session->Routes().Replace(m_boot.startRoute))return false;
    m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    PX_LOG_DEBUG("Player boot: VN runtime constructed");

    // Localization remains a content table; UI resources themselves are fully typed.
    if (auto langText = m_runtime.VFS().ReadText("Content/Localization/" + m_settings.language + ".json")) {
        nlohmann::json j = nlohmann::json::parse(*langText, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_object()&&it.value().contains("translation")&&it.value()["translation"].is_string()) m_langTable[it.key()] = it.value()["translation"].get<std::string>();
            }
            PX_LOG_INFO("Localization: {} entries for '{}'", m_langTable.size(),
                        m_settings.language);
        }
    }
    if (!m_langTable.empty()) {
        m_session->VM().SetTextFilter([this](const std::string& textId,const std::string& text) {
            auto it = m_langTable.find(textId);
            return it != m_langTable.end() ? it->second : text;
        });
    }

    m_luaServices.vfs = &m_runtime.VFS();
    m_luaServices.renderer = &m_runtime.Renderer();
    m_luaServices.audio = &m_runtime.Audio();
    m_luaServices.profile = &m_profile;
    m_luaServices.input = &m_runtime.GetInput();
    m_luaServices.stage = &m_session->Stage();
    m_luaServices.variables = &m_session->Variables();
    m_luaServices.routes = &m_session->Routes();
    m_luaServices.timeline = &m_session->Timeline();
    m_lua = std::make_unique<lua::LuaHost>(m_luaServices);
    (void)m_ui.Actions().RegisterProvider(m_lua->CreateActionProvider());
    m_session->SetExtensionCommandHandler([this](const vn::Command& cmd) {
        // NVL/ADV mode switches are app-level state, handled before Lua.
        const std::string& t = cmd.type;
        if (t == "nvl") {
            m_nvlMode = true;
            if (cmd.Has("clear")) m_nvlLines.clear();
            return true;
        }
        if (t == "adv") {
            m_nvlMode = false;
            m_nvlLines.clear();
            return true;
        }
        if (t == "er" || t == "nvl_clear") {
            m_nvlLines.clear();
            return true;
        }
        const bool handled=m_lua->InvokeCommand(cmd);
        if(handled&&m_lua->HasPendingCommand())m_session->VM().WaitExternal();
        return handled;
    });
    m_session->SetRoutePresentationHandler(
        [this](std::string_view route, std::string_view operation) {
            PresentRoute(std::string(route), std::string(operation));
        });
    if (m_runtime.VFS().Exists("Content/Extensions/extensions.pxindex")) {
        if (!m_lua->LoadExtensionIndex("Content/Extensions/extensions.pxindex")) return false;
    } else if (m_runtime.VFS().Exists("Content/Extensions/default.pxextension")) {
        if (!m_lua->LoadExtensionManifest("Content/Extensions/default.pxextension")) return false;
    }
    m_lua->Emit("engine.ready");
    m_session->VM().SetUnlockHook([this](const std::string& kind, const std::string& id) {
        if (kind == "cg") m_profile.UnlockCG(id);
        else m_profile.UnlockScene(id);
        m_profile.Save("Save/profile.dat", &m_saveKey);
    });
    m_session->VM().SetSeenHook([this](const std::string& key) {
        const bool seen = m_profile.HasSeen(key);
        m_profile.MarkSeen(key);
        return seen;
    });
    m_video = std::make_unique<video::VideoPlayer>(m_runtime.GetWindow().Renderer(),
                                                   m_runtime.VFS());
    m_session->VM().SetVideoHook([this](const std::string& path, bool skippable) {
        // Deferred: opened on the next frame so we never re-enter VM::Run().
        m_pendingVideo = path;
        m_videoSkippable = skippable;
    });

    m_ui.SetActionSink([this](const ui::GalgameAction& action) { HandleUIAction(action); });
    m_session->SetAnimationTargetHandler(animation::TargetKind::UI,[this](const auto& binding,const Variant& value){return m_ui.ApplyAnimationProperty(binding,value);});
    m_session->SetAnimationTargetHandler(animation::TargetKind::Text,[this](const auto& binding,const Variant& value){return m_ui.ApplyAnimationProperty(binding,value);});
    PX_LOG_DEBUG("Player boot: registering typed UI templates");
    const auto registerTemplate=[this](ui::GalgameUI::Screen screen,const std::string& path){
        auto text=m_runtime.VFS().ReadText(path);if(!text){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXPLAYER5003",.category="Player.UI",.message="Required UI template is missing: "+path};d.source.path=path;diag::Emit(d);return false;}
        const Status status=m_ui.RegisterTemplate(screen,*text,path);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return status.IsOk();
    };
    const auto scene=[this](const char* route)->std::string{const auto found=m_boot.routeScenes.find(route);return found==m_boot.routeScenes.end()?std::string{}:found->second;};
    if(!registerTemplate(ui::GalgameUI::Screen::Title,m_boot.routeScenes.at(m_boot.startRoute))||!registerTemplate(ui::GalgameUI::Screen::HUD,scene("hud"))||!registerTemplate(ui::GalgameUI::Screen::Backlog,scene("backlog"))||!registerTemplate(ui::GalgameUI::Screen::Save,scene("save"))||!registerTemplate(ui::GalgameUI::Screen::Load,scene("load"))||!registerTemplate(ui::GalgameUI::Screen::Gallery,scene("gallery"))||!registerTemplate(ui::GalgameUI::Screen::Settings,scene("settings"))||!registerTemplate(ui::GalgameUI::Screen::Video,scene("video")))return false;
    PX_LOG_DEBUG("Player boot: typed UI templates registered");
    if (const Status status = m_ui.ShowTitle(); !status) {
        PX_LOG_CRITICAL("Unable to construct the title UI.");
        return false;
    }
    PX_LOG_DEBUG("Player boot: title UI installed");

    if (auto catalog = m_runtime.VFS().ReadText("Content/Game.pxres")) {
        const Status status = m_catalog.Load(*catalog, "Content/Game.pxres");
        if (!status) return false;
    } else {
        diag::Diagnostic diagnostic{.severity=diag::Severity::Fatal,.code="PXPLAYER5001",.category="Player.Boot",
                                    .message="Required typed GameCatalog is missing: Content/Game.pxres"};
        diag::Emit(diagnostic); return false;
    }
    PX_LOG_DEBUG("Player boot: game catalog loaded");
    for (const auto& binding : m_catalog.InputBindings()) {
        if (binding.command == "screen.open") {
            const int sc = ScancodeFromName(binding.key);
            if (sc != SDL_SCANCODE_UNKNOWN) m_screenTriggers[sc] = binding.argument;
        }
    }
    std::unordered_map<std::string, std::string> voiceDirs;
    for (const auto& ch : m_catalog.Characters()) {
        if (ch.voiceDirectory.empty()) continue;
        if (!ch.id.empty()) voiceDirs[ch.id] = ch.voiceDirectory;
        if (!ch.name.empty()) voiceDirs[ch.name] = ch.voiceDirectory;
    }
    m_session->VM().SetVoiceDirs(std::move(voiceDirs));

    m_script = argc > 1 ? argv[1] : m_boot.startScript;
    return true;
}

void PlayerApp::StartGame() {
    m_session->Variables().Reset(false);
    for (const auto& v : m_catalog.Variables()) {
        m_session->Variables().Set(v.name, v.defaultValue, v.persistent);
    }
    m_session->Backlog().Clear();
    m_autoMode = m_skipMode = m_hudHidden = false;
    m_nvlMode = false;
    m_nvlLines.clear();
    m_rollback.clear();
    m_lastBacklogSize = 0;
    m_playtimeBaseMs = 0;
    m_playtimeStartedAtMs = m_runtime.GetClock().NowMs();
    m_session->VM().LoadScript(m_script);
    if (m_lua) m_lua->Emit("scenario.started", {{"resource", m_script}});
    m_appState = AppState::Game;
    (void)m_session->Routes().Replace("hud");
    m_ui.ShowHUD(DialogueUI());
}

bool PlayerApp::LoadSlot(int slot) {
    auto snap = m_saves.Load(slot);
    if (!snap) {
        return false;
    }
    m_nvlMode = snap->nvlMode;
    m_nvlLines = snap->nvlLines;
    m_rollback.clear();
    RuntimeSession::GameState state;
    state.vm = snap->vm;
    state.dialogue = snap->dialogue;
    state.variables = snap->variables;
    state.typedVariables = snap->typedVariables;
    state.persistentVariables = snap->persistentVariables;
    state.stage = snap->stage;
    state.audio = snap->audio;
    state.backlog = snap->backlog;
    state.routes = snap->routes;
    state.timelines = snap->timelines;
    state.animationClips = snap->animationClips;
    state.behavior = snap->behavior;
    state.playtimeMs = snap->playtimeMs;
    const bool awaitingTimeline = std::any_of(snap->timelines.begin(), snap->timelines.end(),
        [](const animation::PlaybackState& playback) { return playback.playing && playback.awaiting; });
    if ((snap->vm.state == vn::VMState::WaitingExternal) !=
        (!snap->luaPending.empty() || awaitingTimeline)) {
        diag::Emit(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXSAVE6110",
                                    .category="Persistence.Save",
                                    .message="Save has inconsistent Lua await state"});
        return false;
    }
    if (m_lua) {
        const Status luaStatus = m_lua->RestorePending(snap->luaPending);
        if (!luaStatus) return false;
        const Status luaActionStatus = m_lua->RestorePendingActions(snap->luaActions);
        if (!luaActionStatus) return false;
    }
    if (!snap->behavior.fibers.empty() || !snap->behavior.actions.empty()) {
        if (const Status uiStatus = m_ui.ShowHUD(DialogueUI()); !uiStatus) return false;
    }
    if (!m_session->RestoreState(state, m_runtime.GetClock().NowMs())) return false;
    m_lastBacklogSize = m_session->Backlog().Entries().size();
    m_playtimeBaseMs = snap->playtimeMs;
    m_playtimeStartedAtMs = m_runtime.GetClock().NowMs();
    m_appState = AppState::Game;
    m_autoMode = m_skipMode = m_hudHidden = false;
    (void)m_ui.RefreshHUD(DialogueUI());
    if (m_lua) m_lua->Emit("save.loaded", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Loaded slot {}", slot);
    return true;
}

progress::SaveSnapshot PlayerApp::MakeSnapshot(bool includeBacklog) {
    progress::SaveSnapshot snap;
    const std::uint64_t now = m_runtime.GetClock().NowMs();
    snap.playtimeMs = m_playtimeBaseMs +
        (now >= m_playtimeStartedAtMs ? now - m_playtimeStartedAtMs : 0);
    const RuntimeSession::GameState state = m_session->CaptureState(snap.playtimeMs);
    snap.scriptPath = state.vm.scriptPath;
    snap.pc = m_session->VM().SavePoint();
    snap.chapter = state.vm.chapter;
    snap.bgmPath = state.vm.currentBgm;
    snap.stage = state.stage;
    snap.audio = state.audio;
    snap.variables = state.variables;
    snap.typedVariables = state.typedVariables;
    snap.persistentVariables = state.persistentVariables;
    snap.vm = state.vm;
    snap.dialogue = state.dialogue;
    snap.routes = state.routes;
    snap.timelines = state.timelines;
    snap.animationClips = state.animationClips;
    snap.behavior = state.behavior;
    if (m_lua) {
        snap.luaPending = m_lua->CapturePending();
        snap.luaActions = m_lua->CapturePendingActions();
    }
    if (includeBacklog) snap.backlog = state.backlog;
    snap.nvlMode = m_nvlMode;
    snap.nvlLines = m_nvlLines;
    snap.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
    return snap;
}

void PlayerApp::SaveSlot(int slot, std::vector<std::uint8_t> thumbnail) {
    progress::SaveSnapshot snap = MakeSnapshot(/*includeBacklog=*/true);
    snap.thumbnailPng = std::move(thumbnail);
    if (!m_saves.Save(slot, snap)) {
        diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXPLAYER6001",.category="Player.Save",
                           .message="Could not save slot "+std::to_string(slot)};diag::Emit(d);return;
    }
    if (m_lua) m_lua->Emit("save.written", {{"slot", std::to_string(slot)}});
    PX_LOG_INFO("Saved slot {} (thumb {} bytes)", slot, snap.thumbnailPng.size());
}

void PlayerApp::ApplyRollback(const RollbackEntry& entry) {
    const progress::SaveSnapshot& s = entry.snap;
    RuntimeSession::GameState state;
    state.vm = s.vm;
    state.dialogue = s.dialogue;
    state.variables = s.variables;
    state.typedVariables = s.typedVariables;
    state.persistentVariables = s.persistentVariables;
    state.stage = s.stage;
    state.audio = s.audio;
    state.routes = s.routes;
    state.timelines = s.timelines;
    state.animationClips = s.animationClips;
    state.behavior = s.behavior;
    state.backlog = m_session->Backlog().Entries();
    if (state.backlog.size() > entry.backlogSize) state.backlog.resize(entry.backlogSize);
    if (m_lua) {
        const Status luaStatus = m_lua->RestorePending(s.luaPending);
        if (!luaStatus) return;
        const Status luaActionStatus = m_lua->RestorePendingActions(s.luaActions);
        if (!luaActionStatus) return;
    }
    if (!m_session->RestoreState(state, m_runtime.GetClock().NowMs())) return;
    m_nvlMode = s.nvlMode;
    m_nvlLines = s.nvlLines;
    m_lastBacklogSize = m_session->Backlog().Entries().size();
    m_autoMode = m_skipMode = false;
    (void)m_ui.RefreshHUD(DialogueUI());
}

void PlayerApp::RollbackOneLine() {
    if (m_rollback.size() < 2) {
        return;  // back() is the line currently on screen
    }
    m_rollback.pop_back();
    ApplyRollback(m_rollback.back());
}

bool PlayerApp::RollbackToBacklogIndex(std::size_t index) {
    for (std::size_t i = m_rollback.size(); i > 0; --i) {
        const RollbackEntry& entry = m_rollback[i - 1];
        if (entry.backlogSize == index + 1) {
            const RollbackEntry target = entry;
            m_rollback.erase(m_rollback.begin() + static_cast<std::ptrdiff_t>(i),
                             m_rollback.end());
            ApplyRollback(target);
            return true;
        }
    }
    return false;  // line is older than the rollback window
}

std::vector<ui::GalgameItem> PlayerApp::GalleryItems() {
    std::vector<ui::GalgameItem> items;
    for (const auto& cg : m_catalog.Gallery()) {
        const bool unlocked = m_profile.CGUnlocked(cg.id);
        const std::string thumb = cg.thumbnail.empty() ? cg.image : cg.thumbnail;
        items.push_back({cg.id, unlocked ? cg.title : "？？？", unlocked ? "已解鎖" : "尚未解鎖",
                         unlocked ? thumb : "", !unlocked, "cg.view", cg.image});
    }
    return items;
}

std::vector<ui::GalgameItem> PlayerApp::SaveItems(bool saveMode) {
    std::vector<ui::GalgameItem> items;
    const auto slotItem = [&](int slot, const std::string& prefix, const std::string& action) {
        const progress::SlotInfo info = m_saves.Peek(slot);
        std::string label = "空白存檔";
        std::string subtitle;
        std::string image;
        if (info.exists) {
            label = info.chapter.empty() ? "未命名章節" : info.chapter;
            if (info.timestamp != 0) {
                const std::time_t t = static_cast<std::time_t>(info.timestamp);
                char buf[32];
                std::tm localTime{};
#ifdef _WIN32
                const bool converted = localtime_s(&localTime, &t) == 0;
#else
                const bool converted = localtime_r(&t, &localTime) != nullptr;
#endif
                if (converted && std::strftime(buf, sizeof(buf), "  %m/%d %H:%M", &localTime)) {
                    subtitle = buf;
                }
            }
            if (!info.thumbnailPng.empty()) {
                const std::string key = "mem://save/" + std::to_string(slot);
                if (m_runtime.Assets().RegisterMemoryTexture(key, info.thumbnailPng.data(),
                                                             info.thumbnailPng.size())) {
                    image = key;
                }
            }
        }
        items.push_back({"slot-" + std::to_string(slot), prefix + "  " + label, subtitle, image,
                         !saveMode && !info.exists, action, std::to_string(slot)});
    };

    if (!saveMode) {
        slotItem(kAutoSaveSlot, "AUTO", "load.slot");
    }
    for (int i = 0; i < 6; ++i) {
        slotItem(i, "#" + std::to_string(i + 1), saveMode ? "save.slot" : "load.slot");
    }
    return items;
}

std::vector<ui::GalgameItem> PlayerApp::BacklogItems() {
    std::vector<ui::GalgameItem> items;
    const auto& entries = m_session->Backlog().Entries();
    items.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const std::string speaker = entry.isChoice ? "▶ 選擇" : entry.speaker;
        items.push_back({"backlog-" + std::to_string(i), speaker.empty() ? entry.text : speaker + "　" + entry.text,
                         entry.voice.empty() ? "" : "♪ 點擊重播語音", "", false,
                         entry.voice.empty() ? "backlog.rollback" : "backlog.voice", std::to_string(i)});
    }
    return items;
}

ui::DialoguePresentation PlayerApp::DialogueUI() const {
    ui::DialoguePresentation view;
    view.speaker = m_session->Dialogue().State().speaker;
    view.text = m_session->Dialogue().State().displayText;
    view.chapterTitle = m_session->VM().Chapter();
    view.musicTitle = m_session->VM().CurrentBgm();
    view.effect = m_session->Dialogue().State().effect;
    view.effectProgress = m_session->Dialogue().State().effectProgress;
    view.choices = m_choiceTexts;
    view.nvlMode = m_nvlMode;
    view.autoMode = m_autoMode;
    view.skipMode = m_skipMode;
    view.textScale=m_settings.textScale;view.reducedMotion=m_settings.reducedMotion;
    for (const auto& line : m_nvlLines) view.nvlLines.push_back(line.speaker.empty() ? line.text : "【" + line.speaker + "】" + line.text);
    if (!view.nvlLines.empty() && m_session->Dialogue().State().fullText == m_nvlLines.back().text)
        view.nvlLines.back() = m_session->Dialogue().State().speaker.empty() ? m_session->Dialogue().State().displayText : "【" + m_session->Dialogue().State().speaker + "】" + m_session->Dialogue().State().displayText;
    return view;
}

ui::SettingsPresentation PlayerApp::SettingsUI() const {
    return {m_settings.bgmVolume, m_settings.seVolume, m_settings.voiceVolume,
            m_settings.textSpeedMs, m_settings.skipReadOnly, m_settings.fullscreen,
            m_settings.textScale,m_settings.highContrast,m_settings.reducedMotion,m_settings.selfVoicing};
}

void PlayerApp::OpenScreen(const std::string& route) {
    std::string target=route;if(target=="saveload")target="load";
    const Status routed=m_session->Routes().ShowModal(target);if(!routed){for(const auto& diagnostic:routed.Diagnostics())diag::Emit(diagnostic);return;}
    PresentRoute(target, "modal");
}

void PlayerApp::PresentRoute(const std::string& route, const std::string& operation) {
    std::string target=route;if(target=="saveload")target="load";
    if (operation == "back") {
        if (m_appState == AppState::Title) (void)m_ui.ShowTitle();
        else (void)m_ui.ShowHUD(DialogueUI());
        return;
    }
    if (target == "gallery") m_ui.ShowGallery(GalleryItems());
    else if (target == "save") m_ui.ShowSaveLoad(true, SaveItems(true));
    else if (target == "load") m_ui.ShowSaveLoad(false, SaveItems(false));
    else if (target == "settings") m_ui.ShowSettings(SettingsUI());
    else if (target == "backlog") m_ui.ShowBacklog(BacklogItems());
    else if (target == "title") { m_appState = AppState::Title; (void)m_ui.ShowTitle(); }
    else if (target == "hud" || target == "game") { m_appState = AppState::Game; (void)m_ui.ShowHUD(DialogueUI()); }
}

void PlayerApp::HandleUIAction(const ui::GalgameAction& action) {
    const std::string& t = action.command;
    auto& audio = m_runtime.Audio();
    if (t == "load.slot") {
        LoadSlot(std::atoi(action.argument.c_str()));
    } else if (t == "save.slot") {
        SaveSlot(std::atoi(action.argument.c_str()), m_menuThumb);
        m_ui.ShowSaveLoad(true, SaveItems(true));
    } else if (t == "set.bgm.value") {
        m_settings.bgmVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.se.value") {
        m_settings.seVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.voice.value") {
        m_settings.voiceVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.speed.value") {
        m_settings.textSpeedMs=std::clamp(std::atoi(action.argument.c_str()),0,120);m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.skipread.value") {
        m_settings.skipReadOnly=action.argument=="true";
    } else if (t == "set.fullscreen.value") {
        m_settings.fullscreen=action.argument=="true";SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(),m_settings.fullscreen);
    } else if(t=="set.textscale.value") { m_settings.textScale=std::clamp(std::atoi(action.argument.c_str())/100.0f,.75f,2.0f);
    } else if(t=="set.highcontrast.value") { m_settings.highContrast=action.argument=="true";
    } else if(t=="set.reducedmotion.value") { m_settings.reducedMotion=action.argument=="true";
    } else if(t=="set.selfvoicing.value") { m_settings.selfVoicing=action.argument=="true";if(!m_settings.selfVoicing)m_speech.Stop();
    } else if (t == "set.bgm.up") {
        m_settings.bgmVolume = std::min(128, m_settings.bgmVolume + 8);
        audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.bgm.down") {
        m_settings.bgmVolume = std::max(0, m_settings.bgmVolume - 8);
        audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.se.up") {
        m_settings.seVolume = std::min(128, m_settings.seVolume + 8);
        audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.se.down") {
        m_settings.seVolume = std::max(0, m_settings.seVolume - 8);
        audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.voice.up") {
        m_settings.voiceVolume = std::min(128, m_settings.voiceVolume + 8);
        audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.voice.down") {
        m_settings.voiceVolume = std::max(0, m_settings.voiceVolume - 8);
        audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.speed.up") {
        m_settings.textSpeedMs = std::min(120, m_settings.textSpeedMs + 4);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.speed.down") {
        m_settings.textSpeedMs = std::max(0, m_settings.textSpeedMs - 4);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "gallery.open") {
        OpenScreen("gallery");
    } else if (t == "load.open") {
        m_slotSaveMode = false;
        OpenScreen("load");
    } else if (t == "save.open") {
        m_slotSaveMode = true;
        m_pendingSaveScreen = true;
    } else if (t == "settings.open") {
        OpenScreen("settings");
    } else if (t == "cg.view") {
        m_viewingCG = action.argument;
    } else if (t == "set.skipread.toggle") {
        m_settings.skipReadOnly = !m_settings.skipReadOnly;
        m_ui.ShowSettings(SettingsUI());
    } else if (t == "set.fullscreen.toggle") {
        m_settings.fullscreen = !m_settings.fullscreen;
        SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), m_settings.fullscreen);
        m_ui.ShowSettings(SettingsUI());
    } else if (t == "game.start") {
        StartGame();
    } else if (t == "choice.select") {
        m_session->SelectChoice(std::atoi(action.argument.c_str()));
    } else if (t == "mode.auto") {
        m_autoMode = !m_autoMode; if (m_autoMode) m_skipMode = false;
    } else if (t == "mode.skip") {
        m_skipMode = !m_skipMode; if (m_skipMode) m_autoMode = false;
    } else if (t == "backlog.open") {
        OpenScreen("backlog");
    } else if (t == "backlog.voice" || t == "backlog.rollback") {
        const std::size_t index = static_cast<std::size_t>(std::max(0, std::atoi(action.argument.c_str())));
        if (index < m_session->Backlog().Entries().size()) {
            const auto& entry = m_session->Backlog().Entries()[index];
            if (t == "backlog.voice" && !entry.voice.empty())
                m_runtime.Audio().PlayVoice(entry.voice.find('/') != std::string::npos ? entry.voice : m_session->VM().Config().voiceDir + entry.voice);
            else RollbackToBacklogIndex(index);
        }
    } else if (t == "overlay.close") {
        (void)m_session->Routes().CloseModal();
        m_settings.Save("Save/config.dat", &m_saveKey); m_profile.Save("Save/profile.dat", &m_saveKey);
        if (m_appState == AppState::Title) m_ui.ShowTitle(); else m_ui.ShowHUD(DialogueUI());
    } else if (t == "app.quit") {
        m_quitRequested = true;
    }
    if (t.ends_with(".up") || t.ends_with(".down")) m_ui.ShowSettings(SettingsUI());
}

void PlayerApp::ScreensFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0; m_runtime.Renderer().GetLogicalSize(w, h);
    (void)m_ui.Update(input, w, h,dt);
    if (m_appState == AppState::Game) m_session->Stage().Render();
    m_ui.Render(m_runtime.Renderer());
}

void PlayerApp::TitleFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0; m_runtime.Renderer().GetLogicalSize(w, h);
    (void)m_ui.Update(input, w, h,dt);
    m_ui.Render(m_runtime.Renderer());
}

bool PlayerApp::VideoFrame(float dt) {
    if (m_session->VM().State() != vn::VMState::WaitingVideo) {
        return false;
    }
    px::Input& input = m_runtime.GetInput();

    if (!m_pendingVideo.empty()) {
        const float volume = static_cast<float>(m_settings.bgmVolume) / 128.0f;
        if (!m_video->Open(m_pendingVideo, volume)) {
            m_pendingVideo.clear();
            m_session->VM().NotifyVideoDone();
            return false;
        }
        m_pendingVideo.clear();
        // Swallow this frame's input: the click that advanced the script into
        // [video] must not instantly skip the movie.
        int w = 0, h = 0;
        m_runtime.Renderer().GetLogicalSize(w, h);
        m_runtime.Renderer().DrawRect(
            Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) }, Color{ 0, 0, 0, 255 });
        return true;
    }
    if (!m_video->Playing()) {
        m_session->VM().NotifyVideoDone();  // safety: never leave the VM stuck
        return false;
    }

    m_video->Update(dt);
    const bool skipRequested =
        m_videoSkippable &&
        (input.LeftClick() || input.KeyPressed(SDL_SCANCODE_RETURN) ||
         input.KeyPressed(SDL_SCANCODE_ESCAPE) || input.KeyPressed(SDL_SCANCODE_SPACE));
    if (skipRequested || m_video->Finished()) {
        m_video->Close();
        m_session->VM().NotifyVideoDone();
        return false;
    }

    int w = 0, h = 0;
    m_runtime.Renderer().GetLogicalSize(w, h);
    m_runtime.Renderer().DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
                                  Color{ 0, 0, 0, 255 });
    m_video->Render(w, h);
    if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) m_ui.ShowVideoOverlay(m_videoSkippable);
    (void)m_ui.Update(input, w, h,dt);
    m_ui.Render(m_runtime.Renderer());
    return true;
}

void PlayerApp::GameFrame(float dt, std::uint64_t now) {
    px::Input& input = m_runtime.GetInput();

    if (input.KeyPressed(SDL_SCANCODE_F9)) LoadSlot(0);
    if (input.KeyPressed(SDL_SCANCODE_F2)) {
        m_profile.RegisterClear("demo");
        m_session->Variables().Reset(true);
        m_profile.Save("Save/profile.dat", &m_saveKey);
        PX_LOG_INFO("NG+ clear count {}", m_profile.ClearCount());
    }
    if (input.KeyPressed(SDL_SCANCODE_F5)) m_saveRequested = true;
    if (input.KeyPressed(SDL_SCANCODE_F6)) m_pendingSaveScreen = true;

    // Mode toggles (KAG conventions: A=auto, S=skip toggle, Ctrl=skip held).
    if (input.KeyPressed(SDL_SCANCODE_A)) {
        m_autoMode = !m_autoMode;
        if (m_autoMode) m_skipMode = false;
    }
    if (input.KeyPressed(SDL_SCANCODE_S)) {
        m_skipMode = !m_skipMode;
        if (m_skipMode) m_autoMode = false;
    }
    if (input.KeyPressed(SDL_SCANCODE_B) || input.WheelY() > 0.0f) {
        OpenScreen("backlog");
        return;
    }
    if (input.KeyPressed(SDL_SCANCODE_PAGEUP)) {
        RollbackOneLine();
    }
    if (input.RightClick()) {
        m_hudHidden = !m_hudHidden;
    }

    m_choiceTexts.clear();
    if (m_session->VM().State() == vn::VMState::WaitingChoice) {
        for (const auto& c : m_session->VM().Choices()) m_choiceTexts.push_back(c.text);
        m_skipMode = false;  // Skip always stops at choices.
    }

    // Enter/Space advance like a click (KAG convention). Alt+Enter stays the
    // fullscreen toggle handled in MainLoop.
    const bool advanceKey =
        (input.KeyPressed(SDL_SCANCODE_RETURN) && !input.KeyDown(SDL_SCANCODE_LALT) &&
         !input.KeyDown(SDL_SCANCODE_RALT)) ||
        input.KeyPressed(SDL_SCANCODE_SPACE);

    bool advancedByClick = false;
    if (m_hudHidden) {
        if (input.LeftClick() || advanceKey) m_hudHidden = false;
    } else {
        m_ui.RefreshHUD(DialogueUI());
        int uiW = 0, uiH = 0; m_runtime.Renderer().GetLogicalSize(uiW, uiH);
        const bool uiConsumed = m_ui.Update(input, uiW, uiH,dt);
        if (m_ui.IsOverlay()) { m_session->Stage().Render(); m_ui.Render(m_runtime.Renderer()); return; }
        if (!uiConsumed && (input.LeftClick() || advanceKey) &&
               m_session->VM().State() != vn::VMState::WaitingChoice) {
            m_session->Advance();
            advancedByClick = true;
            m_autoMode = false;
            m_skipMode = false;
        }
    }

    // Skip-read-only (既讀スキップ): S-toggle skip stops at unread text;
    // holding Ctrl always force-skips.
    const bool ctrlSkip = input.KeyDown(SDL_SCANCODE_LCTRL) || input.KeyDown(SDL_SCANCODE_RCTRL);
    if (m_skipMode && m_settings.skipReadOnly && !m_session->VM().CurrentLineSeen()) {
        m_skipMode = false;
    }
    const bool skipping = m_skipMode || ctrlSkip;
    if (skipping && !advancedByClick &&
        (m_session->VM().State() == vn::VMState::WaitingClick ||
         m_session->VM().State() == vn::VMState::WaitingTimer)) {
        if (!m_session->Dialogue().Finished()) m_session->Dialogue().ShowAll();
        m_session->Advance();
        m_autoTimerStart = 0;
    } else if (m_autoMode && m_session->VM().State() == vn::VMState::WaitingClick &&
               m_session->Dialogue().Finished()) {
        if (m_autoTimerStart == 0) {
            m_autoTimerStart = now;
        } else if (now - m_autoTimerStart >=
                   static_cast<std::uint64_t>(std::max(0, m_settings.autoWaitMs))) {
            m_session->Advance();
            m_autoTimerStart = 0;
        }
    } else {
        m_autoTimerStart = 0;
    }

    m_session->Update(now, dt);
    if(m_lua){m_lua->Emit("frame.update",{{"delta",std::to_string(dt)}});const bool hadPending=m_lua->HasPendingCommand();m_lua->Update(dt);if(hadPending&&!m_lua->HasPendingCommand())m_session->VM().NotifyExternalDone();}

    // New dialogue lines feed the NVL page and the rollback ring.
    const std::size_t backlogSize = m_session->Backlog().Entries().size();
    if (backlogSize > m_lastBacklogSize) {
        for (std::size_t i = m_lastBacklogSize; i < backlogSize; ++i) {
            const vn::BacklogEntry& e = m_session->Backlog().Entries()[i];
            if (m_nvlMode && !e.isChoice) m_nvlLines.push_back(e);
            if (m_settings.selfVoicing && !e.isChoice)
                m_speech.Speak(e.speaker.empty() ? e.text : e.speaker + ". " + e.text);
        }
        RollbackEntry entry;
        entry.snap = MakeSnapshot(/*includeBacklog=*/false);
        entry.backlogSize = backlogSize;
        m_rollback.push_back(std::move(entry));
        if (m_rollback.size() > 64) m_rollback.pop_front();
        m_lastBacklogSize = backlogSize;
    } else if (backlogSize < m_lastBacklogSize) {
        m_lastBacklogSize = backlogSize;
    }

    m_session->Stage().Render();

    // Autosave once per choice prompt; the stage is rendered, so the thumbnail
    // shows the scene the player was deciding on.
    if (m_session->VM().State() == vn::VMState::WaitingChoice) {
        if (!m_autoSavedChoice) {
            SaveSlot(kAutoSaveSlot,
                     graphics::CaptureThumbnailPng(m_runtime.Renderer().Handle(), 256, 144));
            m_autoSavedChoice = true;
        }
    } else {
        m_autoSavedChoice = false;
    }

    if (m_pendingSaveScreen) {
        m_menuThumb = graphics::CaptureThumbnailPng(m_runtime.Renderer().Handle(), 256, 144);
        m_slotSaveMode = true;
        OpenScreen("save");
        m_pendingSaveScreen = false;
    }

    if (!m_hudHidden) {
        m_ui.RefreshHUD(DialogueUI());
        m_ui.Render(m_runtime.Renderer());
    }

    if (m_saveRequested) {
        SaveSlot(0, graphics::CaptureThumbnailPng(m_runtime.Renderer().Handle(), 256, 144));
        m_saveRequested = false;
    }
}

void PlayerApp::MainLoop() {
    while (!m_quitRequested && m_runtime.BeginFrame()) {
        px::Input& input = m_runtime.GetInput();
        const float dt = m_runtime.GetClock().DeltaSeconds();
        const std::uint64_t now = m_runtime.GetClock().NowMs();

        if (input.KeyPressed(SDL_SCANCODE_RETURN) &&
            (input.KeyDown(SDL_SCANCODE_LALT) || input.KeyDown(SDL_SCANCODE_RALT))) {
            m_settings.fullscreen = !m_settings.fullscreen;
            SDL_SetWindowFullscreen(m_runtime.GetWindow().Handle(), m_settings.fullscreen);
        }

        // Fullscreen CG viewer sits above everything; any click closes it.
        if (!m_viewingCG.empty()) {
            if (input.LeftClick() || input.RightClick() ||
                input.KeyPressed(SDL_SCANCODE_ESCAPE)) {
                m_viewingCG.clear();
            } else {
                int w = 0, h = 0;
                m_runtime.Renderer().GetLogicalSize(w, h);
                m_runtime.Renderer().DrawRect(
                    Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
                    Color{ 0, 0, 0, 255 });
                m_runtime.Renderer().DrawImageAuto(m_viewingCG, graphics::DisplayMode::Fit, 255);
                m_runtime.EndFrame();
                continue;
            }
        }

        if (m_appState == AppState::Game && !m_ui.IsOverlay()) {
            for (const auto& [scancode, route] : m_screenTriggers)
                if (input.KeyPressed(scancode)) OpenScreen(route);
        }

        if (m_ui.IsOverlay() && m_ui.CurrentScreen() != ui::GalgameUI::Screen::Video) {
            if (input.RightClick() || input.KeyPressed(SDL_SCANCODE_ESCAPE)) {
                HandleUIAction({"overlay.close", {}});
            }
            ScreensFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        if (m_appState == AppState::Title) {
            TitleFrame(dt);
            // A title action may replace the UI root. End the frame here so
            // the click that activated Start cannot also advance the VN HUD.
            m_runtime.EndFrame();
            continue;
        }

        if (VideoFrame(dt)) {
            m_runtime.EndFrame();
            continue;
        }

        GameFrame(dt, now);
        m_runtime.EndFrame();
    }
}

void PlayerApp::Shutdown() {
    if (m_lua) m_lua->Emit("engine.shutdown");
    if (!m_settings.fullscreen) {
        int w = 0, h = 0;
        SDL_GetWindowSize(m_runtime.GetWindow().Handle(), &w, &h);
        if (w >= 320 && h >= 180) {
            m_settings.windowWidth = w;
            m_settings.windowHeight = h;
        }
    }
    m_settings.Save("Save/config.dat", &m_saveKey);
    m_profile.Save("Save/profile.dat", &m_saveKey);
    // Subsystems referencing the runtime must go before Runtime::Shutdown.
    m_lua.reset();
    m_session.reset();
    m_runtime.Shutdown();
}

int PlayerApp::Run(int argc, char* argv[]) {
    if (!Init(argc, argv)) {
        return 1;
    }
    MainLoop();
    Shutdown();
    return 0;
}

}
