#include "Applications/Player/PlayerApp.h"

#include "Engine/IO/Crypto.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/UISchema.h"

#include <SDL3/SDL_main.h>

#include <filesystem>
#include <string>

namespace {
const std::string kSaveKey = "prismatix-demo-secret";

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
    px::player::PlayerApp app;
    return app.Run(argc, argv);
}
