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

namespace PrismatiX::App {
void RegisterLuaVNAPI(sol::state& lua, sol::table& api, Engine& engine) {
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

    vn["GetDialogueState"] = [](PrismatiX::VN::VNFlowController& c) -> const PrismatiX::VN::VNDialogueState& { return c.GetDialogueSystem().GetState(); };

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

    api.set_function("CreateVNController", [&engine]() {
        PrismatiX::VN::Commands::VNServices services{ engine.GetResourceManager(), engine.GetGameState(), engine.GetAudioSystem(), engine.GetRenderSystem(), engine.GetChoiceList(), engine.GetLuaState() };
        return std::make_unique<PrismatiX::VN::VNFlowController>(services);
    });
}
}  // namespace PrismatiX::App