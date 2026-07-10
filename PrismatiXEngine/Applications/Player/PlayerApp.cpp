#include "Applications/Player/PlayerApp.h"

#include "Engine/Graphics/Screenshot.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/UISchema.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <utility>

namespace px::player {

namespace {
const std::string kFont = "Data/Font/NotoSansTC-Bold.ttf";
const std::string kSaveKey = "prismatix-demo-secret";
constexpr int kAutoSaveSlot = -1;

bool LoadStage(io::VFS& vfs, const std::string& path, ui::UIStage& stage) {
    auto text = vfs.ReadText(path);
    if (!text) {
        PX_LOG_ERROR("UI scene not found '{}'", path);
        return false;
    }
    auto scene = ui::ParsePXUI(*text);
    if (!scene) {
        PX_LOG_ERROR("UI scene parse failed '{}'", path);
        return false;
    }
    stage.Load(std::move(*scene));
    return true;
}

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
}

PlayerApp::Boot PlayerApp::LoadBootConfig() {
    Boot boot;
    boot.config.title = "PrismatiX Player";
    boot.config.mountDirs = { "." };

    if (std::ifstream mf("game.prismatix"); mf) {
        nlohmann::json j = nlohmann::json::parse(mf, nullptr, false);
        if (!j.is_discarded()) {
            boot.packaged = true;
            boot.config.title = j.value("title", boot.config.title);
            boot.config.mountDirs.clear();
            boot.config.mountArchives = { j.value("archive", std::string{ "Data.pdx" }) };
            if (j.value("encrypt", true)) boot.config.archiveKey = j.value("key", std::string{});
            boot.config.width = boot.config.logicalWidth = j.value("gameWidth", 1280);
            boot.config.height = boot.config.logicalHeight = j.value("gameHeight", 720);
            boot.startUI = j.value("startUi", boot.startUI);
            boot.startScript = j.value("startScript", boot.startScript);
            boot.saveSecret = j.value("key", std::string{});
            if (boot.saveSecret.empty()) boot.saveSecret = boot.config.title;
            PX_LOG_INFO("Packaged build: mounting '{}'", boot.config.mountArchives[0]);
            return boot;
        }
    }

    // Dev run from a project folder: honor the editor's manifest so Start
    // launches the same entry script the Flow editor points at.
    if (std::ifstream pf("project.prismatix.json"); pf) {
        nlohmann::json j = nlohmann::json::parse(pf, nullptr, false);
        if (!j.is_discarded()) {
            boot.config.title = j.value("name", boot.config.title);
            boot.config.width = boot.config.logicalWidth = j.value("gameWidth", 1280);
            boot.config.height = boot.config.logicalHeight = j.value("gameHeight", 720);
            boot.startUI = j.value("startUi", boot.startUI);
            boot.startScript = j.value("startScript", boot.startScript);
            boot.saveSecret = j.value("encryptKey", std::string{});
            if (boot.saveSecret.empty()) boot.saveSecret = j.value("name", std::string{});
            PX_LOG_INFO("Dev run: project '{}', entry '{}'", boot.config.title, boot.startScript);
        }
    } else {
        PX_LOG_WARN("No game.prismatix or project.prismatix.json found; using defaults.");
    }
    return boot;
}

bool PlayerApp::Init(int argc, char* argv[]) {
    m_boot = LoadBootConfig();
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

    m_stage = std::make_unique<vn::Stage>(m_runtime.Renderer(), m_runtime.Assets());
    m_vm = std::make_unique<vn::VM>(m_runtime.VFS(), m_runtime.Audio(), *m_stage, m_dialogue,
                                    m_vars, m_backlog);
    m_vm->SetDefaultTextSpeed(m_settings.textSpeedMs);

    // Localization: Data/Lang/<language>.json maps source line -> translated line.
    if (auto langText = m_runtime.VFS().ReadText("Data/Lang/" + m_settings.language + ".json")) {
        nlohmann::json j = nlohmann::json::parse(*langText, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) m_langTable[it.key()] = it.value().get<std::string>();
            }
            PX_LOG_INFO("Localization: {} entries for '{}'", m_langTable.size(),
                        m_settings.language);
        }
    }
    if (!m_langTable.empty()) {
        m_vm->SetTextFilter([this](const std::string& text) {
            auto it = m_langTable.find(text);
            return it != m_langTable.end() ? it->second : text;
        });
    }

    m_luaServices.vfs = &m_runtime.VFS();
    m_luaServices.renderer = &m_runtime.Renderer();
    m_luaServices.audio = &m_runtime.Audio();
    m_luaServices.profile = &m_profile;
    m_luaServices.input = &m_runtime.GetInput();
    m_luaServices.stage = m_stage.get();
    m_lua = std::make_unique<lua::LuaHost>(m_luaServices);
    m_vm->SetCommandHook([this](const vn::Command& cmd) {
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
        return m_lua->InvokeCommand(cmd);
    });
    if (m_runtime.VFS().Exists("Data/Scripts/extensions.lua")) {
        m_lua->RunFile("Data/Scripts/extensions.lua");
    }
    m_vm->SetUnlockHook([this](const std::string& kind, const std::string& id) {
        if (kind == "cg") m_profile.UnlockCG(id);
        else m_profile.UnlockScene(id);
        m_profile.Save("Save/profile.dat", &m_saveKey);
    });
    m_vm->SetSeenHook([this](const std::string& key) {
        const bool seen = m_profile.HasSeen(key);
        m_profile.MarkSeen(key);
        return seen;
    });
    m_video = std::make_unique<video::VideoPlayer>(m_runtime.GetWindow().Renderer(),
                                                   m_runtime.VFS());
    m_vm->SetVideoHook([this](const std::string& path, bool skippable) {
        // Deferred: opened on the next frame so we never re-enter VM::Run().
        m_pendingVideo = path;
        m_videoSkippable = skippable;
    });

    m_titleOk = LoadStage(m_runtime.VFS(), m_boot.startUI, m_titleStage);
    LoadStage(m_runtime.VFS(), "Data/UI/hud.pxui", m_hud);
    m_hud.SetDialogue(&m_dialogue.State());
    m_hud.SetChoices(&m_choiceTexts);

    m_screens = std::make_unique<ui::ScreenManager>(m_runtime.VFS());

    if (auto dbText = m_runtime.VFS().ReadText("Data/database.json")) {
        if (!m_db.Load(*dbText)) m_db.SeedDefault();
    } else {
        m_db.SeedDefault();
    }
    for (const auto& b : m_db.inputMap) {
        if (b.action == "screen.open") {
            const int sc = ScancodeFromName(b.key);
            if (sc != SDL_SCANCODE_UNKNOWN) m_screens->SetTrigger(sc, b.target);
        }
    }
    std::unordered_map<std::string, std::string> voiceDirs;
    for (const auto& ch : m_db.characters) {
        if (ch.voiceDir.empty()) continue;
        if (!ch.id.empty()) voiceDirs[ch.id] = ch.voiceDir;
        if (!ch.name.empty()) voiceDirs[ch.name] = ch.voiceDir;
    }
    m_vm->SetVoiceDirs(std::move(voiceDirs));

    m_script = argc > 1 ? argv[1] : m_boot.startScript;
    return true;
}

void PlayerApp::StartGame() {
    m_vars.Reset(false);
    for (const auto& v : m_db.variables) m_vars.Set(v.name, v.defaultValue, v.persistent);
    m_backlog.Clear();
    m_autoMode = m_skipMode = m_hudHidden = m_backlogOpen = false;
    m_nvlMode = false;
    m_nvlLines.clear();
    m_rollback.clear();
    m_lastBacklogSize = 0;
    m_vm->LoadScript(m_script);
    m_appState = AppState::Game;
    m_hud.TriggerEnter();
}

bool PlayerApp::LoadSlot(int slot) {
    auto snap = m_saves.Load(slot);
    if (!snap) {
        return false;
    }
    m_vm->SeekTo(snap->scriptPath, snap->pc);
    m_stage->ClearAll();
    if (!snap->bgPath.empty()) m_stage->SetBackground(snap->bgPath, false);
    m_stage->Restore(snap->actors);
    m_stage->RestoreLayers(snap->layers);
    m_vars.Reset(false);
    for (const auto& [k, v] : snap->variables) m_vars.Set(k, v);
    if (!snap->bgmPath.empty()) m_runtime.Audio().PlayBGM(snap->bgmPath, true, 0);
    m_backlog.Clear();
    for (const auto& e : snap->backlog) m_backlog.Push(e.speaker, e.text, e.voice, e.isChoice);
    m_nvlMode = snap->nvlMode;
    m_nvlLines = snap->nvlLines;
    m_rollback.clear();
    m_lastBacklogSize = m_backlog.Entries().size();
    m_vm->Resume();
    m_appState = AppState::Game;
    m_autoMode = m_skipMode = m_hudHidden = m_backlogOpen = false;
    m_hud.TriggerEnter();
    PX_LOG_INFO("Loaded slot {}", slot);
    return true;
}

progress::SaveSnapshot PlayerApp::MakeSnapshot(bool includeBacklog) {
    progress::SaveSnapshot snap;
    snap.scriptPath = m_vm->CurrentScript();
    snap.pc = m_vm->SavePoint();
    snap.chapter = m_vm->Chapter();
    snap.bgPath = m_stage->BackgroundPath();
    snap.bgmPath = m_vm->CurrentBgm();
    snap.actors = m_stage->Snapshot();
    snap.layers = m_stage->SnapshotLayers();
    snap.variables = m_vars.All();
    if (includeBacklog) snap.backlog = m_backlog.Entries();
    snap.nvlMode = m_nvlMode;
    snap.nvlLines = m_nvlLines;
    snap.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
    return snap;
}

void PlayerApp::SaveSlot(int slot, std::vector<std::uint8_t> thumbnail) {
    progress::SaveSnapshot snap = MakeSnapshot(/*includeBacklog=*/true);
    snap.thumbnailPng = std::move(thumbnail);
    m_saves.Save(slot, snap);
    PX_LOG_INFO("Saved slot {} (thumb {} bytes)", slot, snap.thumbnailPng.size());
}

void PlayerApp::ApplyRollback(const RollbackEntry& entry) {
    const progress::SaveSnapshot& s = entry.snap;
    const std::string playingBgm = m_vm->CurrentBgm();
    m_vm->SeekTo(s.scriptPath, s.pc);
    m_stage->ClearAll();
    if (!s.bgPath.empty()) m_stage->SetBackground(s.bgPath, false);
    m_stage->Restore(s.actors);
    m_stage->RestoreLayers(s.layers);
    m_vars.Reset(false);
    for (const auto& [k, v] : s.variables) m_vars.Set(k, v);
    if (s.bgmPath != playingBgm) {
        if (s.bgmPath.empty()) {
            m_runtime.Audio().StopBGM(250);
        } else {
            m_runtime.Audio().PlayBGM(s.bgmPath, true, 250);
        }
        m_vm->OverrideCurrentBgm(s.bgmPath);
    }
    // The re-executed say keeps its existing backlog entry (SeekTo arms the
    // skip), so the log is truncated to include this line and nothing after.
    m_backlog.Truncate(entry.backlogSize);
    m_nvlMode = s.nvlMode;
    m_nvlLines = s.nvlLines;
    m_lastBacklogSize = m_backlog.Entries().size();
    m_autoMode = m_skipMode = false;
    m_backlogOpen = false;
    m_vm->Resume();
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

void PlayerApp::PopulateGallery(ui::UIStage& stage) {
    std::vector<ui::UIStage::GridItem> items;
    for (const auto& cg : m_db.gallery) {
        const bool unlocked = m_profile.CGUnlocked(cg.id);
        const std::string thumb = cg.thumbnail.empty() ? cg.image : cg.thumbnail;
        items.push_back({ cg.title, unlocked ? thumb : "", !unlocked, "cg.view", cg.image });
    }
    stage.SetGrid("gallery", std::move(items));
}

void PlayerApp::PopulateSaves(ui::UIStage& stage) {
    std::vector<ui::UIStage::GridItem> items;
    const auto slotItem = [&](int slot, const std::string& prefix, const std::string& action) {
        const progress::SlotInfo info = m_saves.Peek(slot);
        std::string label = prefix + "  (空)";
        std::string image;
        if (info.exists) {
            label = prefix + "  " + info.chapter;
            if (info.timestamp != 0) {
                const std::time_t t = static_cast<std::time_t>(info.timestamp);
                char buf[32];
                if (std::strftime(buf, sizeof(buf), "  %m/%d %H:%M", std::localtime(&t))) {
                    label += buf;
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
        items.push_back({ label, image, false, action, std::to_string(slot) });
    };

    // The autosave (written at every choice) is load-only.
    if (!m_slotSaveMode) {
        slotItem(kAutoSaveSlot, "AUTO", "load.slot");
    }
    for (int i = 0; i < 6; ++i) {
        slotItem(i, "#" + std::to_string(i), m_slotSaveMode ? "save.slot" : "load.slot");
    }
    stage.SetGrid("saves", std::move(items));
}

void PlayerApp::RefreshSettingsScreen(ui::UIStage& stage) {
    stage.SetNodeText("bgm_val", std::to_string(m_settings.bgmVolume));
    stage.SetNodeText("se_val", std::to_string(m_settings.seVolume));
    stage.SetNodeText("voice_val", std::to_string(m_settings.voiceVolume));
    stage.SetNodeText("speed_val", std::to_string(m_settings.textSpeedMs) + "ms");
    stage.SetNodeText("skipread_val", m_settings.skipReadOnly ? "ON" : "OFF");
}

void PlayerApp::OpenScreen(const std::string& path) {
    if (ui::UIStage* s = m_screens->Open(path)) {
        if (path.find("gallery") != std::string::npos) PopulateGallery(*s);
        else if (path.find("saveload") != std::string::npos) PopulateSaves(*s);
        else if (path.find("settings") != std::string::npos) RefreshSettingsScreen(*s);
    }
}

bool PlayerApp::HandleScreenAction(const ui::UIAction& action) {
    const std::string& t = action.type;
    auto& audio = m_runtime.Audio();
    if (t == "load.slot") {
        m_screens->CloseAll();
        LoadSlot(std::atoi(action.arg.c_str()));
    } else if (t == "save.slot") {
        SaveSlot(std::atoi(action.arg.c_str()), m_menuThumb);
        if (ui::UIStage* top = m_screens->Top()) {
            PopulateSaves(*top);  // show the fresh thumbnail/timestamp
        }
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
        m_vm->SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.speed.down") {
        m_settings.textSpeedMs = std::max(0, m_settings.textSpeedMs - 4);
        m_vm->SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "gallery.open") {
        OpenScreen("Data/UI/gallery.pxui");
    } else if (t == "load.open") {
        m_slotSaveMode = false;
        OpenScreen("Data/UI/saveload.pxui");
    } else if (t == "save.open") {
        m_slotSaveMode = true;
        OpenScreen("Data/UI/saveload.pxui");
    } else if (t == "settings.open") {
        OpenScreen("Data/UI/settings.pxui");
    } else if (t == "cg.view") {
        m_viewingCG = action.arg;
    } else if (t == "set.skipread.toggle") {
        m_settings.skipReadOnly = !m_settings.skipReadOnly;
    } else if (t == "app.quit") {
        return false;
    }
    return true;
}

void PlayerApp::ScreensFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    if (m_screens->Top() && m_screens->TopPath().find("settings") != std::string::npos) {
        RefreshSettingsScreen(*m_screens->Top());
    }
    if (auto act = m_screens->Update(input, dt)) {
        if (!HandleScreenAction(*act)) {
            m_quitRequested = true;
        }
    }
    if (m_screens->Empty()) {
        m_settings.Save("Save/config.dat", &m_saveKey);
        m_profile.Save("Save/profile.dat", &m_saveKey);  // persist read-text flags
    }
    if (m_appState == AppState::Title) {
        m_titleStage.Render(m_runtime.Renderer());
    } else {
        m_stage->Render();
        if (!m_hudHidden) m_hud.Render(m_runtime.Renderer());
    }
    m_screens->Render(m_runtime.Renderer());
}

void PlayerApp::TitleFrame(float dt) {
    px::Input& input = m_runtime.GetInput();
    if (!m_titleOk) {
        // No title screen shipped: never sit on a dead black screen.
        PX_LOG_WARN("Title UI missing — starting '{}' directly.", m_script);
        StartGame();
        return;
    }
    if (auto act = m_titleStage.Update(input, dt)) {
        const std::string& t = act->type;
        if (t == "scene.start") {
            StartGame();
            return;
        }
        if (!HandleScreenAction(*act)) {
            m_quitRequested = true;
        }
    }
    if (m_appState == AppState::Title) {
        m_titleStage.Render(m_runtime.Renderer());
    }
}

void PlayerApp::BacklogFrame() {
    auto& r = m_runtime.Renderer();
    const px::Input& input = m_runtime.GetInput();
    int w = 0, h = 0;
    r.GetLogicalSize(w, h);
    r.DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
               Color{ 8, 10, 16, 225 });
    r.DrawTextOutline("Backlog  (滾輪捲動 / 右鍵關閉 / 點擊重播語音 / Ctrl+點擊回溯)", 40, 24,
                      kFont, 26, Color{ 200, 220, 255, 255 }, Color{ 0, 0, 0, 255 }, 2);

    const auto& entries = m_backlog.Entries();
    const float lineH = 78.0f;
    const float top = 80.0f;
    const float bottom = static_cast<float>(h) - 30.0f;
    const int visible = static_cast<int>((bottom - top) / lineH);
    const int total = static_cast<int>(entries.size());
    const int maxScroll = std::max(0, total - visible);
    m_backlogScroll = std::clamp(m_backlogScroll, 0.0f, static_cast<float>(maxScroll));
    const int first = std::max(0, total - visible - static_cast<int>(m_backlogScroll));

    float y = top;
    for (int i = first; i < total && y < bottom; ++i) {
        const auto& e = entries[static_cast<std::size_t>(i)];
        const bool hovered = input.MouseY() >= y && input.MouseY() < y + lineH;
        if (hovered) {
            r.DrawRect(Rect{ 28, y - 6, static_cast<float>(w) - 56, lineH - 4 },
                       Color{ 60, 90, 130, 70 });
            if (input.LeftClick()) {
                const bool ctrl = input.KeyDown(SDL_SCANCODE_LCTRL) ||
                                  input.KeyDown(SDL_SCANCODE_RCTRL);
                if (ctrl) {
                    if (RollbackToBacklogIndex(static_cast<std::size_t>(i))) {
                        return;  // rollback closed the backlog
                    }
                } else if (!e.voice.empty()) {
                    const std::string& v = e.voice;
                    m_runtime.Audio().PlayVoice(
                        v.find('/') != std::string::npos ? v : m_vm->Config().voiceDir + v);
                }
            }
        }
        const std::string speaker =
            e.isChoice ? "▶ 選擇" : (e.voice.empty() ? e.speaker : e.speaker + "  ♪");
        if (!speaker.empty()) {
            r.DrawTextOutline(speaker, 40, y, kFont, 22, Color{ 255, 220, 140, 255 },
                              Color{ 0, 0, 0, 255 }, 2);
        }
        r.DrawTextOutline(e.text, 40, y + 28, kFont, 24, Color{ 235, 240, 250, 255 },
                          Color{ 0, 0, 0, 255 }, 2, 255, false, w - 80);
        y += lineH;
    }
}

void PlayerApp::RenderNVL() {
    auto& r = m_runtime.Renderer();
    int w = 0, h = 0;
    r.GetLogicalSize(w, h);
    r.DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
               Color{ 10, 12, 18, 215 });
    const float left = 90.0f;
    const float top = 64.0f;
    const int fontSize = 26;
    const float lineGap = 14.0f;
    const int wrap = w - 180;
    float y = top;
    for (std::size_t i = 0; i < m_nvlLines.size(); ++i) {
        const vn::BacklogEntry& e = m_nvlLines[i];
        std::string text = e.text;
        // The last page line is the one currently typing.
        if (i + 1 == m_nvlLines.size() && m_dialogue.State().fullText == e.text) {
            text = m_dialogue.State().displayText;
        }
        const std::string shown = e.speaker.empty() ? text : "【" + e.speaker + "】" + text;
        const std::string full = e.speaker.empty() ? e.text : "【" + e.speaker + "】" + e.text;
        if (!shown.empty()) {
            r.DrawTextOutline(shown, left, y, kFont, fontSize, Color{ 235, 240, 250, 255 },
                              Color{ 0, 0, 0, 255 }, 2, 255, false, wrap);
        }
        // Advance by the full line's height so the layout doesn't jump while typing.
        y += r.MeasureText(full.empty() ? " " : full, kFont, fontSize, wrap).y + lineGap;
        if (y > static_cast<float>(h) - 60.0f) {
            break;
        }
    }
}

bool PlayerApp::VideoFrame(float dt) {
    if (m_vm->State() != vn::VMState::WaitingVideo) {
        return false;
    }
    px::Input& input = m_runtime.GetInput();

    if (!m_pendingVideo.empty()) {
        const float volume = static_cast<float>(m_settings.bgmVolume) / 128.0f;
        if (!m_video->Open(m_pendingVideo, volume)) {
            m_pendingVideo.clear();
            m_vm->NotifyVideoDone();
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
        m_vm->NotifyVideoDone();  // safety: never leave the VM stuck
        return false;
    }

    m_video->Update(dt);
    const bool skipRequested =
        m_videoSkippable &&
        (input.LeftClick() || input.KeyPressed(SDL_SCANCODE_RETURN) ||
         input.KeyPressed(SDL_SCANCODE_ESCAPE) || input.KeyPressed(SDL_SCANCODE_SPACE));
    if (skipRequested || m_video->Finished()) {
        m_video->Close();
        m_vm->NotifyVideoDone();
        return false;
    }

    int w = 0, h = 0;
    m_runtime.Renderer().GetLogicalSize(w, h);
    m_runtime.Renderer().DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) },
                                  Color{ 0, 0, 0, 255 });
    m_video->Render(w, h);
    if (m_videoSkippable) {
        m_runtime.Renderer().DrawTextOutline("點擊跳過 ▶", static_cast<float>(w) - 200, 24, kFont,
                                             20, Color{ 200, 210, 230, 180 },
                                             Color{ 0, 0, 0, 255 }, 2);
    }
    return true;
}

void PlayerApp::GameFrame(float dt, std::uint64_t now) {
    px::Input& input = m_runtime.GetInput();

    if (input.KeyPressed(SDL_SCANCODE_F9)) LoadSlot(0);
    if (input.KeyPressed(SDL_SCANCODE_F2)) {
        m_profile.RegisterClear("demo");
        m_vars.Reset(true);
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
        m_backlogOpen = true;
        m_backlogScroll = 0.0f;
    }
    if (input.KeyPressed(SDL_SCANCODE_PAGEUP)) {
        RollbackOneLine();
    }
    if (input.RightClick()) {
        m_hudHidden = !m_hudHidden;
    }

    m_choiceTexts.clear();
    if (m_vm->State() == vn::VMState::WaitingChoice) {
        for (const auto& c : m_vm->Choices()) m_choiceTexts.push_back(c.text);
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
    } else if (auto act = m_hud.Update(input, dt)) {
        if (act->type == "choice") {
            m_vm->SelectChoice(std::atoi(act->arg.c_str()));
        } else if (act->type == "mode.auto") {
            m_autoMode = !m_autoMode;
            if (m_autoMode) m_skipMode = false;
        } else if (act->type == "mode.skip") {
            m_skipMode = !m_skipMode;
            if (m_skipMode) m_autoMode = false;
        } else if (act->type == "backlog.open") {
            m_backlogOpen = true;
            m_backlogScroll = 0.0f;
        } else if (act->type == "save.open") {
            m_pendingSaveScreen = true;  // deferred so the thumbnail is the clean game frame
        } else {
            HandleScreenAction(*act);
        }
    } else if ((input.LeftClick() || advanceKey) &&
               m_vm->State() != vn::VMState::WaitingChoice) {
        m_vm->OnAdvance();
        advancedByClick = true;
        m_autoMode = false;
        m_skipMode = false;
    }

    // Skip-read-only (既讀スキップ): S-toggle skip stops at unread text;
    // holding Ctrl always force-skips.
    const bool ctrlSkip = input.KeyDown(SDL_SCANCODE_LCTRL) || input.KeyDown(SDL_SCANCODE_RCTRL);
    if (m_skipMode && m_settings.skipReadOnly && !m_vm->CurrentLineSeen()) {
        m_skipMode = false;
    }
    const bool skipping = m_skipMode || ctrlSkip;
    if (skipping && !advancedByClick &&
        (m_vm->State() == vn::VMState::WaitingClick ||
         m_vm->State() == vn::VMState::WaitingTimer)) {
        if (!m_dialogue.Finished()) m_dialogue.ShowAll();
        m_vm->OnAdvance();
        m_autoTimerStart = 0;
    } else if (m_autoMode && m_vm->State() == vn::VMState::WaitingClick &&
               m_dialogue.Finished()) {
        if (m_autoTimerStart == 0) {
            m_autoTimerStart = now;
        } else if (now - m_autoTimerStart >=
                   static_cast<std::uint64_t>(std::max(0, m_settings.autoWaitMs))) {
            m_vm->OnAdvance();
            m_autoTimerStart = 0;
        }
    } else {
        m_autoTimerStart = 0;
    }

    m_vm->Update(now, dt);

    // New dialogue lines feed the NVL page and the rollback ring.
    const std::size_t backlogSize = m_backlog.Entries().size();
    if (backlogSize > m_lastBacklogSize) {
        for (std::size_t i = m_lastBacklogSize; i < backlogSize; ++i) {
            const vn::BacklogEntry& e = m_backlog.Entries()[i];
            if (m_nvlMode && !e.isChoice) m_nvlLines.push_back(e);
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

    m_stage->Render();

    // Autosave once per choice prompt; the stage is rendered, so the thumbnail
    // shows the scene the player was deciding on.
    if (m_vm->State() == vn::VMState::WaitingChoice) {
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
        OpenScreen("Data/UI/saveload.pxui");
        m_pendingSaveScreen = false;
    }

    if (!m_hudHidden) {
        if (m_nvlMode) {
            RenderNVL();
            if (m_vm->State() == vn::VMState::WaitingChoice) {
                m_hud.Render(m_runtime.Renderer());
            }
        } else {
            m_hud.Render(m_runtime.Renderer());
        }

        std::string info = "周目:" + std::to_string(m_profile.ClearCount()) +
                           "  [F5 存檔][F9 讀檔][A 自動][S 快進][B 紀錄][PgUp 回溯]";
        if (m_autoMode) info += "  ● AUTO";
        if (skipping) info += "  ▶▶ SKIP";
        m_runtime.Renderer().DrawTextOutline(info, 20, 16, kFont, 22,
                                             Color{ 210, 230, 255, 255 },
                                             Color{ 0, 0, 0, 255 }, 2);
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

        if (m_appState == AppState::Game && !m_backlogOpen) {
            m_screens->HandleTriggers(input);
        }

        if (!m_screens->Empty()) {
            ScreensFrame(dt);
            m_runtime.EndFrame();
            continue;
        }

        if (m_appState == AppState::Title) {
            TitleFrame(dt);
            if (m_appState == AppState::Title) {
                m_runtime.EndFrame();
                continue;
            }
        }

        if (VideoFrame(dt)) {
            m_runtime.EndFrame();
            continue;
        }

        if (m_backlogOpen) {
            if (input.WheelY() != 0.0f) m_backlogScroll += input.WheelY() * 3.0f;
            if (input.RightClick() || input.KeyPressed(SDL_SCANCODE_B) ||
                input.KeyPressed(SDL_SCANCODE_ESCAPE)) {
                m_backlogOpen = false;
            }
            m_stage->Render();
            BacklogFrame();
            m_runtime.EndFrame();
            continue;
        }

        GameFrame(dt, now);
        m_runtime.EndFrame();
    }
}

void PlayerApp::Shutdown() {
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
    m_vm.reset();
    m_stage.reset();
    m_screens.reset();
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
