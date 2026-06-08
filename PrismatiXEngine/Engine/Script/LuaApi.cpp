#include "Engine/Audio/AudioEngine.h"
#include "Engine/Gfx/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Script/LuaHost.h"
#include "Logger.h"

namespace px::script {

void LuaHost::BindApi() {
    sol::table api = m_lua.create_table();

    api.set_function("log", [](const std::string& msg) { PX_LOG_INFO("[lua] {}", msg); });

    api.set_function("RegisterCommand", [this](const std::string& name, sol::protected_function fn) {
        m_commands[name] = std::move(fn);
    });
    api.set_function("On", [this](const std::string& event, sol::protected_function fn) {
        m_bus.Subscribe(event, [this, fn](const EventArgs& args) {
            sol::table t = m_lua.create_table();
            for (const auto& [k, v] : args) t[k] = v;
            sol::protected_function_result r = fn(t);
            if (!r.valid()) {
                const sol::error e = r;
                PX_LOG_ERROR("[lua] event handler error: {}", e.what());
            }
        });
    });
    api.set_function("Emit", [this](const std::string& event, sol::optional<sol::table> payload) {
        EventArgs args;
        if (payload) {
            for (const auto& kv : *payload) {
                args[kv.first.as<std::string>()] = kv.second.as<std::string>();
            }
        }
        m_bus.Emit(event, args);
    });

    if (progress::GlobalProfile* p = m_services.profile) {
        api.set_function("HasSeen", [p](const std::string& k) { return p->HasSeen(k); });
        api.set_function("MarkSeen", [p](const std::string& k) { p->MarkSeen(k); });
        api.set_function("ClearCount", [p]() { return p->ClearCount(); });
        api.set_function("CGUnlocked", [p](const std::string& id) { return p->CGUnlocked(id); });
        api.set_function("SceneUnlocked",
                         [p](const std::string& id) { return p->SceneUnlocked(id); });
        api.set_function("UnlockCG", [p](const std::string& id) { p->UnlockCG(id); });
        api.set_function("UnlockScene", [p](const std::string& id) { p->UnlockScene(id); });
        api.set_function("PersistentVar",
                         [p](const std::string& n) { return p->PersistentVar(n); });
    }

    if (audio::AudioEngine* a = m_services.audio) {
        api.set_function("PlaySe", [a](const std::string& path) { a->PlaySe(path); });
        api.set_function("PlayBgm", [a](const std::string& path, sol::optional<bool> loop,
                                        sol::optional<int> fade) {
            a->PlayBgm(path, loop.value_or(true), fade.value_or(0));
        });
        api.set_function("StopBgm", [a](sol::optional<int> fade) { a->StopBgm(fade.value_or(0)); });
        api.set_function("SetBgmVolume", [a](int v) { a->SetBgmVolume(v); });
        api.set_function("SetSeVolume", [a](int v) { a->SetSeVolume(v); });
        api.set_function("SetVoiceVolume", [a](int v) { a->SetVoiceVolume(v); });
    }

    if (px::Input* in = m_services.input) {
        api.set_function("GetMouseX", [in]() { return in->MouseX(); });
        api.set_function("GetMouseY", [in]() { return in->MouseY(); });
        api.set_function("GetLeftClick", [in]() { return in->LeftClick(); });
        api.set_function("GetRightClick", [in]() { return in->RightClick(); });
    }

    if (gfx::Renderer2D* r = m_services.renderer) {
        api.set_function("GetLogicalSize", [r]() {
            int w = 0, h = 0;
            r->GetLogicalSize(w, h);
            return std::make_tuple(w, h);
        });
        api.set_function("DrawImage", [r](const std::string& path, float x, float y, float w,
                                          float h, sol::optional<int> alpha) {
            r->DrawImage(path, Rect{ x, y, w, h },
                         static_cast<std::uint8_t>(alpha.value_or(255)));
        });
        api.set_function("DrawAuto", [r](const std::string& path, int mode, sol::optional<int> a) {
            r->DrawImageAuto(path, static_cast<gfx::DisplayMode>(mode),
                             static_cast<std::uint8_t>(a.value_or(255)));
        });
        api.set_function("DrawRect", [r](float x, float y, float w, float h, int cr, int cg, int cb,
                                         int ca) {
            r->DrawRect(Rect{ x, y, w, h },
                        Color{ (std::uint8_t)cr, (std::uint8_t)cg, (std::uint8_t)cb,
                               (std::uint8_t)ca });
        });
        api.set_function("DrawRoundedRect", [r](float x, float y, float w, float h, float radius,
                                                int cr, int cg, int cb, int ca) {
            r->DrawRoundedRect(Rect{ x, y, w, h }, radius,
                               Color{ (std::uint8_t)cr, (std::uint8_t)cg, (std::uint8_t)cb,
                                      (std::uint8_t)ca });
        });
        api.set_function("DrawText", [r](const std::string& text, float x, float y,
                                         const std::string& font, int size, int cr, int cg, int cb,
                                         sol::optional<int> ca) {
            r->DrawText(text, x, y, font, size,
                        Color{ (std::uint8_t)cr, (std::uint8_t)cg, (std::uint8_t)cb,
                               (std::uint8_t)ca.value_or(255) });
        });
        api.set_function("MeasureText", [this, r](const std::string& text, const std::string& font,
                                                  int size) {
            const Vec2 v = r->MeasureText(text, font, size);
            sol::table t = m_lua.create_table();
            t["w"] = v.x;
            t["h"] = v.y;
            return t;
        });
    }

    sol::table dm = m_lua.create_table();
    dm["TopLeft"] = static_cast<int>(gfx::DisplayMode::TopLeft);
    dm["Center"] = static_cast<int>(gfx::DisplayMode::Center);
    dm["Fit"] = static_cast<int>(gfx::DisplayMode::Fit);
    dm["Fill"] = static_cast<int>(gfx::DisplayMode::Fill);
    dm["Bottom"] = static_cast<int>(gfx::DisplayMode::Bottom);
    dm["FitWidthBottom"] = static_cast<int>(gfx::DisplayMode::FitWidthBottom);
    m_lua["DisplayMode"] = dm;

    m_lua["Engine"] = api;
    m_lua["px"] = api;
}

}
