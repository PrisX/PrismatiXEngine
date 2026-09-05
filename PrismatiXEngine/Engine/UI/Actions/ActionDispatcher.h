#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Actions/ActionCatalog.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace px::ui {

using ActionExecutionId = std::uint64_t;
enum class ActionExecutionState : std::uint8_t {
    Running,
    Completed,
    Failed,
    Cancelled,
    Unknown,
};

struct ProviderActionStart {
    Status status;
    std::uint64_t handle = 0;
    bool pending = false;
};

class IActionProvider {
public:
    virtual ~IActionProvider() = default;
    [[nodiscard]] virtual std::string_view ProviderId() const = 0;
    [[nodiscard]] virtual ActionOrigin Origin() const = 0;
    [[nodiscard]] virtual bool CanInvoke(std::string_view action) const = 0;
    virtual Status Invoke(const ActionInvocation& invocation) = 0;
    virtual ProviderActionStart Start(const ActionInvocation& invocation) {
        return {.status = Invoke(invocation)};
    }
    [[nodiscard]] virtual ActionExecutionState Poll(std::uint64_t) const {
        return ActionExecutionState::Completed;
    }
    virtual void Cancel(std::uint64_t) {}
    virtual void Update(float) {}
};

struct ActionDispatchOptions {
    bool previewSafeMode = false;
    std::optional<ActionReentryPolicy> reentryPolicy;
};

// Serializable bridge between a Behavior fiber and the provider coroutine it
// is awaiting. Providers restore their own handle checkpoints first; the
// dispatcher then rebuilds the public execution handles referenced by fibers.
struct ActionExecutionCheckpoint {
    ActionExecutionId execution = 0;
    ActionInvocation invocation;
    std::string providerId;
    std::uint64_t providerHandle = 0;
    bool autoForget = false;
};

class ActionDispatcher {
public:
    explicit ActionDispatcher(ActionCatalog& catalog = ActionCatalog::Global()) : m_catalog(catalog) {}

    Status RegisterProvider(std::shared_ptr<IActionProvider> provider);
    Status UnregisterProvider(std::string_view providerId);
    [[nodiscard]] std::shared_ptr<IActionProvider> FindProvider(std::string_view providerId) const;
    [[nodiscard]] Status Dispatch(ActionInvocation invocation,
                                  ActionDispatchOptions options = {});
    [[nodiscard]] Result<ActionExecutionId> Start(ActionInvocation invocation,
                                                   ActionDispatchOptions options = {});
    [[nodiscard]] ActionExecutionState State(ActionExecutionId execution) const;
    [[nodiscard]] Status Failure(ActionExecutionId execution) const;
    Status Cancel(ActionExecutionId execution);
    void CancelAll();
    void CancelSource(std::string_view sourceScene);
    void Forget(ActionExecutionId execution);
    void Update(float deltaSeconds);
    [[nodiscard]] std::vector<ActionExecutionCheckpoint> CaptureState() const;
    [[nodiscard]] Status ValidateState(
        const std::vector<ActionExecutionCheckpoint>& state) const;
    Status RestoreState(const std::vector<ActionExecutionCheckpoint>& state);

    [[nodiscard]] ActionCatalog& Catalog() { return m_catalog; }
    [[nodiscard]] const ActionCatalog& Catalog() const { return m_catalog; }

private:
    ActionCatalog& m_catalog;
    std::unordered_map<std::string, std::shared_ptr<IActionProvider>> m_providers;
    struct Execution {
        ActionExecutionId id = 0;
        std::string action;
        std::shared_ptr<IActionProvider> provider;
        std::uint64_t providerHandle = 0;
        ActionExecutionState state = ActionExecutionState::Unknown;
        Status failure{};
        bool autoForget = false;
        ActionInvocation invocation;
    };
    std::unordered_map<ActionExecutionId, Execution> m_executions;
    ActionExecutionId m_nextExecution = 1;
};

}  // namespace px::ui
