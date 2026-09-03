#pragma once

#include "Engine/Script/ScriptHost.h"

#include <memory>

namespace px::script {

// Embedded, Node-free JavaScript backend. QuickJS implementation details stay
// behind the pimpl so the runtime contract does not expose a vendor ABI.
class JavaScriptHost final : public ScriptHost {
public:
    explicit JavaScriptHost(const ScriptServices& services);
    ~JavaScriptHost() override;

    JavaScriptHost(const JavaScriptHost&) = delete;
    JavaScriptHost& operator=(const JavaScriptHost&) = delete;

    [[nodiscard]] std::string_view BackendId() const noexcept override {
        return "javascript";
    }

    bool RunFile(const std::string& vfsPath);
    bool RunString(const std::string& source,
                   const std::string& sourceName = "<script>");
    bool LoadExtensionManifest(const std::string& manifestPath) override;
    bool LoadExtensionIndex(const std::string& indexPath) override;
    Status Emit(const std::string& event,
                const EventPayload& payload = {}) override;
    bool InvokeCommand(const vn::Command& command) override;
    [[nodiscard]] std::shared_ptr<ui::IActionProvider> CreateActionProvider() override;
    void Update(float deltaSeconds) override;
    [[nodiscard]] bool HasPendingCommand() const override;
    [[nodiscard]] bool HasPendingAction() const override;
    [[nodiscard]] PendingCommandsState CapturePending() const override;
    Status RestorePending(const PendingCommandsState& state) override;
    [[nodiscard]] PendingActionsState CapturePendingActions() const override;
    Status RestorePendingActions(const PendingActionsState& state) override;
    [[nodiscard]] Result<ExtensionStates> CaptureExtensionState() override;
    Status RestoreExtensionState(const ExtensionStates& state) override;
    void CancelPending() override;
    std::vector<DebugBreakpoint> SetDebugBreakpoints(
        std::vector<DebugBreakpoint> breakpoints) override;
    bool DebugPause() override;
    bool DebugContinue() override;
    bool DebugStep() override;
    [[nodiscard]] std::optional<DebugVariable> EvaluateDebugWatch(
        std::string_view expression) const override;
    [[nodiscard]] const DebugSnapshot& CaptureDebugState() const override;

    [[nodiscard]] bool HasAction(std::string_view action) const;
    Status InvokeAction(const ui::ActionInvocation& invocation);
    ui::ProviderActionStart StartAction(const ui::ActionInvocation& invocation);
    [[nodiscard]] ui::ActionExecutionState ActionState(std::uint64_t handle) const;
    void CancelAction(std::uint64_t handle);

private:
    bool RunModuleFile(const std::string& vfsPath);
    bool LoadExtensionManifestIntoCurrentRealm(const std::string& manifestPath);
    Status RestorePendingInCurrentRealm(const PendingCommandsState& state);
    Status RestorePendingActionsInCurrentRealm(const PendingActionsState& state);
    [[nodiscard]] Result<ExtensionStates> CaptureExtensionStateInCurrentRealm();
    Status RestoreExtensionStateInCurrentRealm(const ExtensionStates& state);
    Status RestoreExtensionStateUnchecked(const ExtensionStates& state);
    Status DispatchEventInCurrentRealm(const std::string& event,
                                       const EventPayload& payload);

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace px::script
