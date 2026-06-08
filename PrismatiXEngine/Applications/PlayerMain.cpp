#include "Engine/Support/Logger.h"
#include "Engine/Runtime.h"
#include "Engine/Graphics/Screenshot.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Project/Database.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/UI/ScreenManager.h"
#include "Engine/UI/UISchema.h"
#include "Engine/UI/UIStage.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {
const std::string kFont = "Data/Font/NotoSansTC-Bold.ttf";
const std::string kSaveKey = "prismatix-demo-secret";

bool LoadStage(px::io::VFS& vfs, const std::string& path, px::ui::UIStage& stage) {
    auto text = vfs.ReadText(path);
    if (!text) {
        PX_LOG_ERROR("UI scene not found '{}'", path);
        return false;
    }
    auto scene = px::ui::ParsePXUI(*text);
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

int RunSelfTest() {
    const px::crypto::Key key = px::crypto::DeriveKey(kSaveKey);
    int failures = 0;
    auto check = [&](const char* what, bool ok) {
        PX_LOG_INFO("[selftest] {} : {}", what, ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    };

    {
        px::crypto::Bytes plain = { 'h', 'i', 1, 2, 3 };
        auto enc = px::crypto::Encrypt(plain, key, px::crypto::DeriveIv("s"));
        check("crypto AES round-trip",
              px::crypto::Decrypt(enc, key, px::crypto::DeriveIv("s")) == plain && enc != plain);
    }
    {
        px::progress::SaveSystem saves;
        saves.Configure("Save", &key);
        px::progress::SaveSnapshot s;
        s.scriptPath = "chapter1.pds";
        s.pc = 42;
        s.chapter = "序章";
        s.variables = { { "love", 3 } };
        s.actors = { { "girl", "Data/Image/Character/girl_d.png", 2 } };
        s.thumbnailPng = { 1, 2, 3, 4 };
        saves.Save(9, s);
        auto l = saves.Load(9);
        check("save slot round-trip", l && l->pc == 42 && l->variables.at("love") == 3 &&
                                          l->actors.size() == 1 && l->thumbnailPng == s.thumbnailPng);
        saves.Delete(9);
    }
    {
        px::progress::GlobalProfile p;
        p.RegisterClear("end");
        p.UnlockCG("cg_01");
        p.SetPersistentVar("ng", 5);
        p.Save("Save/_st_profile.dat", &key);
        px::progress::GlobalProfile q;
        q.Load("Save/_st_profile.dat", &key);
        check("profile round-trip",
              q.ClearCount() == 1 && q.CGUnlocked("cg_01") && q.PersistentVar("ng") == 5);
        std::error_code ec;
        std::filesystem::remove("Save/_st_profile.dat", ec);
    }
    {
        px::progress::GlobalProfile p;
        px::lua::LuaServices services;
        services.profile = &p;
        px::lua::LuaHost lua(services);
        lua.RunString("Engine.RegisterCommand('greet', function(a) Engine.UnlockCG('cg_'..a.id) end)\n"
                      "Engine.On('ping', function(a) Engine.UnlockScene('scene_'..a.who) end)\n");
        lua.InvokeCommand(px::vn::Command{ "greet", { { "id", "42" } }, 0 });
        lua.Emit("ping", { { "who", "bob" } });
        check("lua command + event", p.CGUnlocked("cg_42") && p.SceneUnlocked("scene_bob"));
    }
    {
        const std::string text =
            R"({"canvas":{"w":1280,"h":720},"nodes":[{"id":"b","type":"button","rect":[10,20,300,64],"text":"hi","actionType":"scene.start"}]})";
        auto scene = px::ui::ParsePXUI(text);
        bool ok = scene && scene->nodes.size() == 1 &&
                  scene->nodes[0].type == px::ui::NodeType::Button && scene->nodes[0].id == "b";
        if (ok) {
            auto again = px::ui::ParsePXUI(px::ui::WritePXUI(*scene));
            ok = again && again->nodes.size() == 1 && again->nodes[0].actionType == "scene.start" &&
                 again->nodes[0].rect.w == 300.0f;
        }
        check("pxui parse round-trip", ok);
    }

    PX_LOG_INFO("[selftest] {} ({} failures)", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
}

int main(int argc, char* argv[]) {
    Logger::Initialize();
    if (argc > 1 && std::string(argv[1]) == "--selftest") {
        return RunSelfTest();
    }

    px::Runtime runtime;
    px::RuntimeConfig config;
    config.title = "PrismatiX Player";
    config.mountDirs = { "." };

    std::string startUI = "Data/UI/title.pxui";
    std::string startScript = "test_scene.pds";
    if (std::ifstream mf("game.prismatix"); mf) {
        nlohmann::json j = nlohmann::json::parse(mf, nullptr, false);
        if (!j.is_discarded()) {
            config.title = j.value("title", config.title);
            config.mountDirs.clear();
            config.mountArchives = { j.value("archive", std::string{ "Data.pdx" }) };
            if (j.value("encrypt", true)) config.archiveKey = j.value("key", std::string{});
            config.width = config.logicalWidth = j.value("gameWidth", 1280);
            config.height = config.logicalHeight = j.value("gameHeight", 720);
            startUI = j.value("startUi", startUI);
            startScript = j.value("startScript", startScript);
            PX_LOG_INFO("Packaged build: mounting '{}'", config.mountArchives[0]);
        }
    }

    if (!runtime.Init(config)) {
        PX_LOG_CRITICAL("Failed to initialize runtime.");
        return 1;
    }

    const px::crypto::Key saveKey = px::crypto::DeriveKey(kSaveKey);
    px::progress::GlobalProfile profile;
    profile.Load("Save/profile.dat", &saveKey);
    px::progress::GameSettings settings;
    settings.Load("Save/config.dat", &saveKey);
    px::progress::SaveSystem saves;
    saves.Configure("Save", &saveKey);
    runtime.Audio().SetBGMVolume(settings.bgmVolume);
    runtime.Audio().SetSEVolume(settings.seVolume);
    runtime.Audio().SetVoiceVolume(settings.voiceVolume);

    px::vn::Stage stage(runtime.Renderer(), runtime.Assets());
    px::vn::Dialogue dialogue;
    px::vn::VariableStore vars;
    px::vn::Backlog backlog;
    px::vn::VM vm(runtime.VFS(), runtime.Audio(), stage, dialogue, vars, backlog);

    px::lua::LuaServices luaServices;
    luaServices.vfs = &runtime.VFS();
    luaServices.renderer = &runtime.Renderer();
    luaServices.audio = &runtime.Audio();
    luaServices.profile = &profile;
    luaServices.input = &runtime.GetInput();
    px::lua::LuaHost lua(luaServices);
    vm.SetCommandHook([&lua](const px::vn::Command& cmd) { return lua.InvokeCommand(cmd); });
    if (runtime.VFS().Exists("Data/Scripts/extensions.lua")) {
        lua.RunFile("Data/Scripts/extensions.lua");
    }
    vm.SetUnlockHook([&](const std::string& kind, const std::string& id) {
        if (kind == "cg") profile.UnlockCG(id);
        else profile.UnlockScene(id);
        profile.Save("Save/profile.dat", &saveKey);
    });

    px::ui::UIStage titleStage, hud;
    LoadStage(runtime.VFS(), startUI, titleStage);
    LoadStage(runtime.VFS(), "Data/UI/hud.pxui", hud);
    std::vector<std::string> choiceTexts;
    hud.SetDialogue(&dialogue.State());
    hud.SetChoices(&choiceTexts);

    px::ui::ScreenManager screens(runtime.VFS());

    px::project::Database db;
    if (auto dbText = runtime.VFS().ReadText("Data/database.json")) {
        if (!db.Load(*dbText)) db.SeedDefault();
    } else {
        db.SeedDefault();
    }
    for (const auto& b : db.inputMap) {
        if (b.action == "screen.open") {
            const int sc = ScancodeFromName(b.key);
            if (sc != SDL_SCANCODE_UNKNOWN) screens.SetTrigger(sc, b.target);
        }
    }

    auto populateGallery = [&](px::ui::UIStage& s) {
        std::vector<px::ui::UIStage::GridItem> items;
        for (const auto& cg : db.gallery) {
            const bool unlocked = profile.CGUnlocked(cg.id);
            const std::string thumb = cg.thumbnail.empty() ? cg.image : cg.thumbnail;
            items.push_back({ cg.title, unlocked ? thumb : "", !unlocked, "cg.view", cg.image });
        }
        s.SetGrid("gallery", std::move(items));
    };
    auto populateSaves = [&](px::ui::UIStage& s) {
        std::vector<px::ui::UIStage::GridItem> items;
        for (int i = 0; i < 6; ++i) {
            const px::progress::SlotInfo info = saves.Peek(i);
            const std::string label = info.exists ? ("#" + std::to_string(i) + "  " + info.chapter)
                                                  : ("#" + std::to_string(i) + "  (空)");
            items.push_back({ label, "", false, "load.slot", std::to_string(i) });
        }
        s.SetGrid("saves", std::move(items));
    };
    auto refreshSettings = [&](px::ui::UIStage& s) {
        s.SetNodeText("bgm_val", std::to_string(settings.bgmVolume));
        s.SetNodeText("se_val", std::to_string(settings.seVolume));
        s.SetNodeText("voice_val", std::to_string(settings.voiceVolume));
        s.SetNodeText("speed_val", std::to_string(settings.textSpeedMs) + "ms");
    };

    enum class AppState { Title, Game };
    AppState appState = AppState::Title;
    auto openScreen = [&](const std::string& path) {
        if (px::ui::UIStage* s = screens.Open(path)) {
            if (path.find("gallery") != std::string::npos) populateGallery(*s);
            else if (path.find("saveload") != std::string::npos) populateSaves(*s);
            else if (path.find("settings") != std::string::npos) refreshSettings(*s);
        }
    };
    const std::string script = argc > 1 ? argv[1] : startScript;
    bool saveRequested = false;

    auto loadSlot = [&](int slot) {
        if (auto snap = saves.Load(slot)) {
            vm.SeekTo(snap->scriptPath, snap->pc);
            stage.ClearAll();
            if (!snap->bgPath.empty()) stage.SetBackground(snap->bgPath, false);
            stage.Restore(snap->actors);
            vars.Reset(false);
            for (const auto& [k, v] : snap->variables) vars.Set(k, v);
            if (!snap->bgmPath.empty()) runtime.Audio().PlayBGM(snap->bgmPath, true, 0);
            vm.Resume();
            appState = AppState::Game;
            hud.TriggerEnter();
        }
    };

    while (runtime.BeginFrame()) {
        px::Input& input = runtime.GetInput();
        const float dt = runtime.GetClock().DeltaSeconds();
        const std::uint64_t now = runtime.GetClock().NowMs();

        if (appState == AppState::Game) {
            screens.HandleTriggers(input);
        }

        if (!screens.Empty()) {
            if (screens.Top() && screens.TopPath().find("settings") != std::string::npos) {
                refreshSettings(*screens.Top());
            }
            if (auto act = screens.Update(input, dt)) {
                const std::string& t = act->type;
                if (t == "load.slot") {
                    screens.CloseAll();
                    loadSlot(std::atoi(act->arg.c_str()));
                } else if (t == "set.bgm.up") {
                    settings.bgmVolume = std::min(128, settings.bgmVolume + 8);
                    runtime.Audio().SetBGMVolume(settings.bgmVolume);
                } else if (t == "set.bgm.down") {
                    settings.bgmVolume = std::max(0, settings.bgmVolume - 8);
                    runtime.Audio().SetBGMVolume(settings.bgmVolume);
                } else if (t == "set.se.up") {
                    settings.seVolume = std::min(128, settings.seVolume + 8);
                    runtime.Audio().SetSEVolume(settings.seVolume);
                } else if (t == "set.se.down") {
                    settings.seVolume = std::max(0, settings.seVolume - 8);
                    runtime.Audio().SetSEVolume(settings.seVolume);
                } else if (t == "set.voice.up") {
                    settings.voiceVolume = std::min(128, settings.voiceVolume + 8);
                    runtime.Audio().SetVoiceVolume(settings.voiceVolume);
                } else if (t == "set.voice.down") {
                    settings.voiceVolume = std::max(0, settings.voiceVolume - 8);
                    runtime.Audio().SetVoiceVolume(settings.voiceVolume);
                } else if (t == "set.speed.up") {
                    settings.textSpeedMs = std::min(120, settings.textSpeedMs + 4);
                } else if (t == "set.speed.down") {
                    settings.textSpeedMs = std::max(0, settings.textSpeedMs - 4);
                } else if (t == "gallery.open") {
                    openScreen("Data/UI/gallery.pxui");
                } else if (t == "load.open") {
                    openScreen("Data/UI/saveload.pxui");
                } else if (t == "settings.open") {
                    openScreen("Data/UI/settings.pxui");
                }
            }
            if (screens.Empty()) {
                settings.Save("Save/config.dat", &saveKey);
            }
            if (appState == AppState::Title) {
                titleStage.Render(runtime.Renderer());
            } else {
                stage.Render();
                hud.Render(runtime.Renderer());
            }
            screens.Render(runtime.Renderer());
            runtime.EndFrame();
            continue;
        }

        if (appState == AppState::Title) {
            if (auto act = titleStage.Update(input, dt)) {
                const std::string& t = act->type;
                if (t == "scene.start") {
                    vars.Reset(false);
                    for (const auto& v : db.variables) vars.Set(v.name, v.defaultValue, v.persistent);
                    vm.LoadScript(script);
                    appState = AppState::Game;
                    hud.TriggerEnter();
                } else if (t == "gallery.open") {
                    openScreen("Data/UI/gallery.pxui");
                } else if (t == "settings.open") {
                    openScreen("Data/UI/settings.pxui");
                } else if (t == "load.open") {
                    openScreen("Data/UI/saveload.pxui");
                } else if (t == "app.quit") {
                    break;
                }
            }
            titleStage.Render(runtime.Renderer());
        } else {
            if (input.KeyPressed(SDL_SCANCODE_F9)) {
                if (auto snap = saves.Load(0)) {
                    vm.SeekTo(snap->scriptPath, snap->pc);
                    stage.ClearAll();
                    if (!snap->bgPath.empty()) stage.SetBackground(snap->bgPath, false);
                    stage.Restore(snap->actors);
                    vars.Reset(false);
                    for (const auto& [k, v] : snap->variables) vars.Set(k, v);
                    if (!snap->bgmPath.empty()) runtime.Audio().PlayBGM(snap->bgmPath, true, 0);
                    vm.Resume();
                    PX_LOG_INFO("Loaded slot 0");
                }
            }
            if (input.KeyPressed(SDL_SCANCODE_F2)) {
                profile.RegisterClear("demo");
                vars.Reset(true);
                profile.Save("Save/profile.dat", &saveKey);
                PX_LOG_INFO("NG+ clear count {}", profile.ClearCount());
            }
            if (input.KeyPressed(SDL_SCANCODE_F5)) saveRequested = true;

            choiceTexts.clear();
            if (vm.State() == px::vn::VMState::WaitingChoice) {
                for (const auto& c : vm.Choices()) choiceTexts.push_back(c.text);
            }

            if (auto act = hud.Update(input, dt)) {
                if (act->type == "choice") vm.SelectChoice(std::atoi(act->arg.c_str()));
            } else if (input.LeftClick() && vm.State() != px::vn::VMState::WaitingChoice) {
                vm.OnAdvance();
            }

            vm.Update(now, dt);
            stage.Render();
            hud.Render(runtime.Renderer());

            const std::string info = "周目:" + std::to_string(profile.ClearCount()) +
                                     "  route_a:" + std::to_string(vars.Get("route_a")) +
                                     "  [F5 存檔][F9 讀檔][F2 新周目]";
            runtime.Renderer().DrawTextOutline(info, 20, 16, kFont, 22, px::Color{ 210, 230, 255, 255 },
                                               px::Color{ 0, 0, 0, 255 }, 2);

            if (saveRequested) {
                px::progress::SaveSnapshot snap;
                snap.scriptPath = vm.CurrentScript();
                snap.pc = vm.ProgramCounter();
                snap.chapter = vm.Chapter();
                snap.bgPath = stage.BackgroundPath();
                snap.bgmPath = vm.CurrentBgm();
                snap.actors = stage.Snapshot();
                snap.variables = vars.All();
                snap.backlog = backlog.Entries();
                snap.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
                snap.thumbnailPng = px::graphics::CaptureThumbnailPng(runtime.Renderer().Handle(), 256, 144);
                saves.Save(0, snap);
                PX_LOG_INFO("Saved slot 0 (thumb {} bytes)", snap.thumbnailPng.size());
                saveRequested = false;
            }
        }

        runtime.EndFrame();
    }

    settings.Save("Save/config.dat", &saveKey);
    runtime.Shutdown();
    return 0;
}
