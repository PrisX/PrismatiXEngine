#pragma once

#include "Engine/Lua/EventBus.h"
#include "Engine/Lua/LuaState.h"
#include "Engine/Script/ScriptHost.h"

#include <sol/sol.hpp>

#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_Debug;
struct lua_State;

namespace px::lua {

using LuaConsoleLevel = script::ConsoleLevel;
using LuaConsoleMessage = script::ConsoleMessage;
using LuaServices = script::ScriptServices;

class LuaHost final : public script::ScriptHost {
public:
    using DebugBreakpoint = script::DebugBreakpoint;
    using DebugVariable = script::DebugVariable;
    using DebugFrame = script::DebugFrame;
    using DebugSnapshot = script::DebugSnapshot;

    struct ExtensionManifest {
        std::string id;
        int order = 0;
        std::string entry;
        std::vector<std::string> capabilities;
        std::unordered_set<std::string> commands;
        std::unordered_set<std::string> actions;
    };

    explicit LuaHost(const LuaServices& services);
    ~LuaHost() override;

    bool RunFile(const std::string& vfsPath);
    bool RunString(const std::string& code, const std::string& chunkName = "chunk");
    [[nodiscard]] std::string_view BackendId() const noexcept override { return "lua"; }
    bool LoadExtensionManifest(const std::string& manifestPath) override;
    bool LoadExtensionIndex(const std::string& indexPath) override;

    EventBus& Bus() { return m_bus; }
    void Emit(const std::string& event, const script::EventArgs& args = {}) override {
        m_bus.Emit(event, args);
    }

    bool InvokeCommand(const vn::Command& cmd) override;
    Status InvokeAction(const ui::ActionInvocation& invocation);
    ui::ProviderActionStart StartAction(const ui::ActionInvocation& invocation);
    [[nodiscard]] ui::ActionExecutionState ActionState(std::uint64_t handle) const;
    void CancelAction(std::uint64_t handle);
    [[nodiscard]] bool HasAction(std::string_view action) const;
    [[nodiscard]] std::shared_ptr<ui::IActionProvider> CreateActionProvider() override;
    void Update(float deltaSeconds) override;
    [[nodiscard]] bool HasPendingCommand() const override { return !m_pending.empty(); }
    [[nodiscard]] bool HasPendingAction() const override { return !m_pendingActions.empty(); }
    [[nodiscard]] PendingCommandsState CapturePending() const override;
    Status RestorePending(const PendingCommandsState& state) override;
    [[nodiscard]] PendingActionsState CapturePendingActions() const override;
    Status RestorePendingActions(const PendingActionsState& state) override;
    void CancelPending() override;
    std::vector<DebugBreakpoint> SetDebugBreakpoints(
        std::vector<DebugBreakpoint> breakpoints) override;
    bool DebugPause() override;
    bool DebugContinue() override;
    bool DebugStep() override;
    [[nodiscard]] std::optional<DebugVariable> EvaluateDebugWatch(
        std::string_view expression) const override;
    [[nodiscard]] const DebugSnapshot& CaptureDebugState() const override {
        return m_debugSnapshot;
    }

    sol::state& State() { return m_lua; }
    [[nodiscard]] const LuaServices& Services() const { return m_services; }

private:
    void BindApi();
    void BindVfsRequire();
    void HandleError(const std::string& where, const std::string& message);
    void EmitConsole(LuaConsoleLevel level, std::string message,
                     std::string source = {}, int line = 0) const;
    static int ConsolePrint(lua_State* state);
    static int ConsoleWarn(lua_State* state);
    void PrepareDebugHook(lua_State* state);
    void CaptureDebugStack(lua_State* state, lua_Debug* event,
                           std::string reason);
    static void DebugHook(lua_State* state, lua_Debug* event);

    sol::state m_lua;
    sol::thread m_runner;
    EventBus m_bus;
    LuaServices m_services;
    std::unordered_map<std::string, sol::protected_function> m_commands;
    std::unordered_map<std::string, sol::protected_function> m_actions;
    std::unordered_map<std::string, sol::object> m_modules;
    std::unordered_set<std::string> m_declaredCommands;
    std::unordered_set<std::string> m_declaredActions;
    std::unordered_set<std::string> m_loadedActionSources;
    std::string m_activeExtension;
    struct PendingCoroutine {
        std::shared_ptr<sol::thread> runner;
        sol::coroutine coroutine;
        std::string waitKind;
        std::uint64_t handle = 0;
        float remainingSeconds = 0.0f;
        vn::Command command;
        std::uint32_t yieldIndex = 0;
    };
    std::vector<PendingCoroutine> m_pending;
    struct PendingActionCoroutine {
        std::shared_ptr<sol::thread> runner;
        sol::coroutine coroutine;
        std::uint64_t id = 0;
        std::string action;
        ui::ActionInvocation invocation;
        std::string waitKind;
        std::uint64_t handle = 0;
        float remainingSeconds = 0.0f;
        std::uint32_t yieldIndex = 0;
    };
    std::vector<PendingActionCoroutine> m_pendingActions;
    std::unordered_map<std::uint64_t, ui::ActionExecutionState> m_actionTerminalStates;
    std::uint64_t m_nextActionHandle = 1;
    std::unordered_map<std::string, std::unordered_set<int>> m_debugBreakpoints;
    DebugSnapshot m_debugSnapshot;
    bool m_debugPauseRequested = false;
    bool m_debugStepRequested = false;
    std::string m_debugSkipSource;
    int m_debugSkipLine = 0;
    lua_State* m_debugPausedState = nullptr;
};

}
