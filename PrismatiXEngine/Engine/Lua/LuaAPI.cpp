#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Support/Logger.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/IO/VFS.h"

#include <algorithm>

namespace px::lua {
namespace {
vn::Value LuaToValue(const sol::object& object, const int depth = 0) {
    if (depth > 32) throw sol::error("Lua value nesting exceeds 32 levels");
    if (!object.valid() || object.is<sol::nil_t>()) return {};
    if (object.is<bool>()) return object.as<bool>();
    if (object.is<std::int64_t>()) return object.as<std::int64_t>();
    if (object.is<double>()) return object.as<double>();
    if (object.is<std::string>()) return object.as<std::string>();
    if (!object.is<sol::table>()) throw sol::error("unsupported Lua value type");
    const sol::table table = object.as<sol::table>();
    const std::size_t sequenceSize = table.size();
    bool sequence = true;
    for (const auto& pair : table) {
        if (!pair.first.is<std::int64_t>()) { sequence = false; break; }
        const auto index = pair.first.as<std::int64_t>();
        if (index < 1 || static_cast<std::size_t>(index) > sequenceSize) { sequence = false; break; }
    }
    if (sequence) {
        vn::ValueList values; values.reserve(sequenceSize);
        for (std::size_t index = 1; index <= sequenceSize; ++index)
            values.push_back(LuaToValue(table.get<sol::object>(index), depth + 1));
        return values;
    }
    vn::ValueMap values;
    for (const auto& pair : table) {
        if (!pair.first.is<std::string>()) throw sol::error("map keys must be strings");
        values.emplace(pair.first.as<std::string>(), LuaToValue(pair.second, depth + 1));
    }
    return values;
}

sol::object ValueToLua(sol::state_view lua, const vn::Value& value, const int depth = 0) {
    if (depth > 32) throw sol::error("runtime value nesting exceeds 32 levels");
    if (const auto* item=value.TryGet<bool>()) return sol::make_object(lua,*item);
    if (const auto* item=value.TryGet<std::int64_t>()) return sol::make_object(lua,*item);
    if (const auto* item=value.TryGet<double>()) return sol::make_object(lua,*item);
    if (const auto* item=value.TryGet<std::string>()) return sol::make_object(lua,*item);
    if (const auto* items=value.AsArray()) { sol::table table=lua.create_table(static_cast<int>(items->size()),0);std::size_t index=1;for(const auto& item:*items)table[index++]=ValueToLua(lua,item,depth+1);return sol::make_object(lua,table); }
    if (const auto* items=value.AsObject()) { sol::table table=lua.create_table();for(const auto& [key,item]:*items)table[key]=ValueToLua(lua,item,depth+1);return sol::make_object(lua,table); }
    return sol::make_object(lua, sol::nil);
}
}

void LuaHost::BindApi() {
    sol::table api = m_lua.create_table();

    api.set_function("log", [](const std::string& msg) { PX_LOG_INFO("[lua] {}", msg); });

    api.set_function("RegisterCommand", [this](const std::string& name, sol::protected_function fn) {
        if (!m_activeExtension.empty() && !m_declaredCommands.contains(name)) {
            throw sol::error("command '" + name + "' is not declared by extension '" +
                             m_activeExtension + "'");
        }
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

    if (vn::VariableStore* variables = m_services.variables) {
        api.set_function("GetVariable", [this, variables](const std::string& name) -> sol::object {
            const auto* value=variables->GetValue(name);if(!value)return sol::make_object(m_lua,sol::nil);
            return ValueToLua(m_lua,*value);
        });
        api.set_function("SetVariable", [variables](const std::string& name,sol::object object,sol::optional<std::string> scope){vn::Value value=LuaToValue(object);const std::string selected=scope.value_or("save");if(selected!="save"&&selected!="persistent"&&selected!="temporary")throw sol::error("unknown variable scope: "+selected);variables->SetValue(name,std::move(value),selected=="persistent"?vn::VariableScope::Persistent:selected=="temporary"?vn::VariableScope::Temporary:vn::VariableScope::SaveLocal);});
    }

    if(ui::UIRouter* routes=m_services.routes){api.set_function("PushRoute",[routes](const std::string& route){const Status status=routes->Push(route);if(!status)throw sol::error(diag::Describe(status.Diagnostics().front()));});api.set_function("ReplaceRoute",[routes](const std::string& route){const Status status=routes->Replace(route);if(!status)throw sol::error(diag::Describe(status.Diagnostics().front()));});api.set_function("BackRoute",[routes](){return static_cast<bool>(routes->Back());});api.set_function("ShowModal",[routes](const std::string& route){return static_cast<bool>(routes->ShowModal(route));});api.set_function("CloseModal",[routes](){return static_cast<bool>(routes->CloseModal());});}

    if(animation::TimelinePlayer* timeline=m_services.timeline){api.set_function("LoadAnimation",[this,timeline](const std::string& path){if(!m_services.vfs)throw sol::error("VFS unavailable");const auto text=m_services.vfs->ReadText(path);if(!text)throw sol::error("animation not found: "+path);auto clip=animation::ParseAnimationClip(*text,path);if(!clip)throw sol::error(diag::Describe(clip.Diagnostics().front()));const auto id=clip.Value().id;const Status status=timeline->Register(clip.TakeValue());if(!status)throw sol::error(diag::Describe(status.Diagnostics().front()));return id.ToString();});api.set_function("PlayAnimation",[timeline](const std::string& id,sol::optional<bool> await,sol::optional<float> speed){const auto parsed=Uuid::Parse(id);if(!parsed)throw sol::error("invalid animation ResourceId");return timeline->Play(*parsed,await.value_or(false),speed.value_or(1.0f));});api.set_function("CancelAnimation",[timeline](std::uint64_t handle){return static_cast<bool>(timeline->Cancel(handle));});api.set_function("AwaitAnimation",sol::yielding([](std::uint64_t handle){return std::make_tuple(std::string("animation"),handle);}));}
    api.set_function("AwaitSeconds",sol::yielding([](float seconds){return std::make_tuple(std::string("timer"),std::max(0.0f,seconds));}));
    if (m_services.vfs) {
        api.set_function("ResourceExists", [this](const std::string& path) {
            return m_services.vfs->Exists(path);
        });
        api.set_function("ReadResourceText", [this](const std::string& path) {
            const auto text = m_services.vfs->ReadText(path);
            if (!text) throw sol::error("resource not found or not text: " + path);
            return *text;
        });
    }

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
        api.set_function("PlaySE", [a](const std::string& path) { a->PlaySE(path); });
        api.set_function("PlayBGM", [a](const std::string& path, sol::optional<bool> loop,
                                        sol::optional<int> fade) {
            a->PlayBGM(path, loop.value_or(true), fade.value_or(0));
        });
        api.set_function("StopBGM", [a](sol::optional<int> fade) { a->StopBGM(fade.value_or(0)); });
        api.set_function("SetBGMVolume", [a](int v) { a->SetBGMVolume(v); });
        api.set_function("SetSEVolume", [a](int v) { a->SetSEVolume(v); });
        api.set_function("SetVoiceVolume", [a](int v) { a->SetVoiceVolume(v); });
        api.set_function("SetAmbienceVolume", [a](int v) { a->SetAmbienceVolume(v); });
        api.set_function("PlayAmbience", [a](const std::string& path, sol::optional<bool> loop,
                                               sol::optional<int> fade) {
            a->PlayAmbience(path, loop.value_or(true), fade.value_or(0));
        });
        api.set_function("StopAmbience", [a](sol::optional<int> fade) {
            a->StopAmbience(fade.value_or(0));
        });
    }

    // VN stage control: the high-level escape hatch for custom commands that go
    // beyond the built-in typed command set (custom poses, effects, scripted scenes).
    if (vn::Stage* s = m_services.stage) {
        api.set_function("SetBackground",
                         [s](const std::string& path, sol::optional<bool> transition) {
                             s->SetBackground(path, transition.value_or(true));
                         });
        api.set_function("SetCharacter",
                         [s](const std::string& name, const std::string& image,
                             sol::optional<int> slot, sol::optional<bool> transition,
                             sol::optional<float> x, sol::optional<float> y,
                             sol::optional<float> scale) {
                             s->SetCharacter(name, image, slot.value_or(2),
                                             transition.value_or(true), x.value_or(0.0f),
                                             y.value_or(0.0f), scale.value_or(1.0f));
                         });
        api.set_function("ClearCharacter",
                         [s](const std::string& name, sol::optional<bool> transition) {
                             s->ClearCharacter(name, transition.value_or(true));
                         });
        api.set_function("MoveCharacter",
                         [s](const std::string& name, int slot) { s->MoveCharacter(name, slot); });
        api.set_function("SetLayer",
                         [s](const std::string& name, const std::string& image,
                             sol::optional<float> x, sol::optional<float> y,
                             sol::optional<float> scale, sol::optional<int> alpha,
                             sol::optional<int> z) {
                             s->SetLayer(name, image, x.value_or(0.0f), y.value_or(0.0f),
                                         scale.value_or(1.0f),
                                         static_cast<std::uint8_t>(
                                             std::clamp(alpha.value_or(255), 0, 255)),
                                         z.value_or(0));
                         });
        api.set_function("ClearLayer", [s](const std::string& name) { s->ClearLayer(name); });
        api.set_function("Shake", [s](sol::optional<int> ms, sol::optional<float> amp) {
            s->Shake(ms.value_or(400), amp.value_or(12.0f));
        });
        // Engine.Animate("girl", { x=120, scale=1.1, alpha=255, duration=600, ease="outBack" })
        api.set_function("Animate", [s](const std::string& target, sol::table props) {
            vn::Stage::TweenSpec spec;
            if (auto v = props.get<sol::optional<float>>("x")) {
                spec.hasX = true;
                spec.x = *v;
            }
            if (auto v = props.get<sol::optional<float>>("y")) {
                spec.hasY = true;
                spec.y = *v;
            }
            if (auto v = props.get<sol::optional<float>>("scale")) {
                spec.hasScale = true;
                spec.scale = *v;
            }
            if (auto v = props.get<sol::optional<float>>("alpha")) {
                spec.hasAlpha = true;
                spec.alpha = *v;
            }
            spec.durationMs = props.get_or("duration", 600);
            spec.ease = props.get_or("ease", std::string{ "outCubic" });
            return s->Animate(target, spec);
        });
    }

    if (px::Input* in = m_services.input) {
        api.set_function("GetMouseX", [in]() { return in->MouseX(); });
        api.set_function("GetMouseY", [in]() { return in->MouseY(); });
        api.set_function("GetLeftClick", [in]() { return in->LeftClick(); });
        api.set_function("GetRightClick", [in]() { return in->RightClick(); });
    }

    if (graphics::Renderer2D* r = m_services.renderer) {
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
            r->DrawImageAuto(path, static_cast<graphics::DisplayMode>(mode),
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
    dm["TopLeft"] = static_cast<int>(graphics::DisplayMode::TopLeft);
    dm["Center"] = static_cast<int>(graphics::DisplayMode::Center);
    dm["Fit"] = static_cast<int>(graphics::DisplayMode::Fit);
    dm["Fill"] = static_cast<int>(graphics::DisplayMode::Fill);
    dm["Bottom"] = static_cast<int>(graphics::DisplayMode::Bottom);
    dm["FitWidthBottom"] = static_cast<int>(graphics::DisplayMode::FitWidthBottom);
    m_lua["DisplayMode"] = dm;

    m_lua["Engine"] = api;
    m_lua["px"] = api;
}

}
