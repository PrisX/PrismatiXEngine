#include "Core/Engine.h"
#include "Core/Lua/LuaAPI.h"
#include "Core/Lua/LuaUtils.h"
#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Systems/AudioSystem.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/VN/Commands/VNContext.h"
#include "Core/VN/VNChoiceList.h"
#include "Core/VN/VNFlowController.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace PrismatiX::App {
namespace {
std::unordered_map<lua_State*, std::vector<std::shared_ptr<PrismatiX::VN::VNFlowController>>> g_vnControllerKeepAlive;

sol::table CreateDialogueStateTable(sol::state_view lua, const PrismatiX::VN::VNDialogueState& state) {
    sol::table table = lua.create_table();
    table["speaker"] = state.speaker;
    table["fullText"] = state.fullText;
    table["currentText"] = state.currentDisplayText;
    table["currentIndex"] = state.currentIndex;
    table["totalChars"] = state.totalChars;
    table["isFinished"] = state.isFinished;
    table["effect"] = state.activeEffect;
    table["effectProgress"] = state.effectProgress;
    return table;
}

sol::table CreateVNControllerProxy(
    sol::state_view lua,
    const std::shared_ptr<PrismatiX::VN::VNFlowController>& controller,
    Engine& engine) {
    sol::table proxy = lua.create_table();
    proxy["_px_vn_ptr"] = controller.get();

    proxy.set_function("LoadScript", [controller](sol::table, const std::string& name) {
        controller->LoadScript(name);
    });
    proxy.set_function("Update", [controller](sol::table, float mouseX, float mouseY) {
        controller->Update(LInt(mouseX), LInt(mouseY));
    });
    proxy.set_function("Render", [controller](sol::table) {
        controller->Render();
    });
    proxy.set_function("RenderBackground", [controller](sol::table) {
        controller->GetStage().Render();
    });
    proxy.set_function("HandleClick", [controller](sol::table, float mouseX, float mouseY) {
        controller->HandleClick(LInt(mouseX), LInt(mouseY));
    });
    proxy.set_function("GetDialogueState", [controller](sol::table, sol::this_state state) {
        return CreateDialogueStateTable(sol::state_view(state), controller->GetDialogueSystem().GetState());
    });
    proxy.set_function("PopPendingBgmInfo", [controller](sol::table, sol::this_state state) -> sol::object {
        std::string str = controller->PopPendingBgm();
        if (str.empty()) {
            return sol::lua_nil;
        }
        return sol::make_object(state, str);
    });
    proxy.set_function("PopPendingChapterInfo", [controller](sol::table, sol::this_state state) -> sol::object {
        std::string str = controller->PopPendingChapter();
        if (str.empty()) {
            return sol::lua_nil;
        }
        return sol::make_object(state, str);
    });
    proxy.set_function("GetChoices", [&engine](sol::table, sol::this_state state) {
        sol::state_view luaState(state);
        sol::table result = luaState.create_table();
        const auto& choices = engine.GetChoiceList().GetChoices();
        for (size_t index = 0; index < choices.size(); ++index) {
            sol::table item = luaState.create_table();
            item["text"] = choices[index].text;
            item["target"] = choices[index].target;
            result[index + 1] = item;
        }
        return result;
    });
    proxy.set_function("SelectChoice", [controller](sol::table, int index) {
        controller->SelectChoice(index - 1);
    });

    return proxy;
}
}

void RegisterLuaVNAPI(sol::state& lua, sol::table& api, Engine& engine) {
    g_vnControllerKeepAlive.erase(lua.lua_state());

    lua.new_usertype<PrismatiX::VN::VNDialogueState>(
        "VNDialogueState",
        "speaker",
        &PrismatiX::VN::VNDialogueState::speaker,
        "fullText",
        &PrismatiX::VN::VNDialogueState::fullText,
        "currentText",
        &PrismatiX::VN::VNDialogueState::currentDisplayText,
        "currentIndex",
        &PrismatiX::VN::VNDialogueState::currentIndex,
        "totalChars",
        &PrismatiX::VN::VNDialogueState::totalChars,
        "isFinished",
        &PrismatiX::VN::VNDialogueState::isFinished,
        "effect",
        &PrismatiX::VN::VNDialogueState::activeEffect,
        "effectProgress",
        &PrismatiX::VN::VNDialogueState::effectProgress
    );

    auto vn = lua.new_usertype<PrismatiX::VN::VNFlowController>("VNController", sol::no_constructor);
    vn["LoadScript"] = [](PrismatiX::VN::VNFlowController& c, const std::string& n) { c.LoadScript(n); };
    vn["Update"] = [](PrismatiX::VN::VNFlowController& c, float mx, float my) { c.Update(LInt(mx), LInt(my)); };
    vn["Render"] = [](PrismatiX::VN::VNFlowController& c) { c.Render(); };
    vn["RenderBackground"] = [](PrismatiX::VN::VNFlowController& c) { c.GetStage().Render(); };
    vn["HandleClick"] = [](PrismatiX::VN::VNFlowController& c, float mx, float my) { c.HandleClick(LInt(mx), LInt(my)); };

    vn["GetDialogueState"] = [](PrismatiX::VN::VNFlowController& c, sol::this_state state) {
        return CreateDialogueStateTable(sol::state_view(state), c.GetDialogueSystem().GetState());
    };

    vn["PopPendingBgmInfo"] = [](PrismatiX::VN::VNFlowController& c, sol::this_state s) -> sol::object {
        std::string str = c.PopPendingBgm();
        if (str.empty()) return sol::lua_nil;
        return sol::make_object(s, str);
    };
    vn["PopPendingChapterInfo"] = [](PrismatiX::VN::VNFlowController& c, sol::this_state s) -> sol::object {
        std::string str = c.PopPendingChapter();
        if (str.empty()) return sol::lua_nil;
        return sol::make_object(s, str);
    };
    vn["GetChoices"] = [&engine](PrismatiX::VN::VNFlowController& c, sol::this_state s) {
        sol::state_view lua(s);
        sol::table res = lua.create_table();
        const auto& choices = engine.GetChoiceList().GetChoices();
        for (size_t i = 0; i < choices.size(); ++i) {
            sol::table item = lua.create_table();
            item["text"] = choices[i].text;
            item["target"] = choices[i].target;
            res[i + 1] = item;
        }
        return res;
    };
    vn["SelectChoice"] = [](PrismatiX::VN::VNFlowController& c, int i) { c.SelectChoice(i - 1); };

    api.set_function("CreateVNController", [&engine](sol::this_state s) {
        PrismatiX::VN::Commands::VNServices services{ engine.GetResourceManager(), engine.GetGameState(), engine.GetAudioSystem(), engine.GetRenderSystem(), engine.GetChoiceList(), engine.GetLuaState() };
        auto controller = std::make_shared<PrismatiX::VN::VNFlowController>(services);
        sol::state_view luaState(s);
        g_vnControllerKeepAlive[luaState.lua_state()].push_back(controller);
        return CreateVNControllerProxy(luaState, controller, engine);
    });
}
}  // namespace PrismatiX::App
