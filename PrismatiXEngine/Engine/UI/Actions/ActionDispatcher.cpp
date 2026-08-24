#include "Engine/UI/Actions/ActionDispatcher.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <cmath>
#include <unordered_set>

namespace px::ui {
namespace {

diag::Diagnostic DispatchError(std::string code, std::string message,
                               const ActionInvocation* invocation = nullptr) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "UI.Action",
                                .message = std::move(message)};
    if (invocation) {
        diagnostic.source.path = invocation->context.sourceScene;
        diagnostic.source.nodeId = invocation->context.sourceNode.Empty()
                                       ? std::string{} : invocation->context.sourceNode.ToString();
        diagnostic.source.property = "triggers." + invocation->context.signal;
    }
    return diagnostic;
}

Status EmitFailure(diag::Diagnostic diagnostic) {
    diag::Emit(diagnostic);
    return Status::Fail(std::move(diagnostic));
}

Status EmitFailure(const std::vector<diag::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) diag::Emit(diagnostic);
    return Status::Fail(diagnostics);
}

}  // namespace

Status ActionDispatcher::RegisterProvider(std::shared_ptr<IActionProvider> provider) {
    if (!provider || provider->ProviderId().empty())
        return Status::Fail(DispatchError("PXUIACTION2020", "Action provider id is empty"));
    const std::string id(provider->ProviderId());
    if (m_providers.contains(id))
        return Status::Fail(DispatchError("PXUIACTION2021", "Duplicate action provider: " + id));
    m_providers.emplace(id, std::move(provider));
    return Status::Ok();
}

Status ActionDispatcher::UnregisterProvider(const std::string_view providerId) {
    m_providers.erase(std::string(providerId));
    return Status::Ok();
}

std::shared_ptr<IActionProvider> ActionDispatcher::FindProvider(const std::string_view providerId) const {
    const auto found = m_providers.find(std::string(providerId));
    return found == m_providers.end() ? nullptr : found->second;
}

Result<ActionExecutionId> ActionDispatcher::Start(ActionInvocation invocation,
                                                   const ActionDispatchOptions options) {
    if (invocation.action.empty())
        return Result<ActionExecutionId>::Failure(DispatchError(
            "PXUIACTION2022", "Action invocation has no id", &invocation));
    const auto* descriptor = m_catalog.Find(invocation.action);
    if (!descriptor)
        return Result<ActionExecutionId>::Failure(DispatchError(
            "PXUIACTION2010", "Missing action: " + invocation.action, &invocation));
    if (!descriptor->available)
        return Result<ActionExecutionId>::Failure(DispatchError("PXUIACTION2011",
            "Action is unavailable: " + invocation.action +
            (descriptor->unavailableReason.empty() ? std::string{} : " (" + descriptor->unavailableReason + ")"),
            &invocation));
    if (options.previewSafeMode &&
        (descriptor->destructiveInPreview || !descriptor->previewSafe ||
         !descriptor->deterministic))
        return Result<ActionExecutionId>::Failure(DispatchError("PXUIACTION2023",
            "Action is blocked by its Preview safety contract: " + invocation.action,
            &invocation));

    auto normalized = m_catalog.ValidateAndNormalize(invocation);
    if (!normalized) return Result<ActionExecutionId>::Failure(normalized.Diagnostics());
    invocation.arguments = normalized.TakeValue();

    ActionExecutionId running = 0;
    std::vector<ActionExecutionId> runningExecutions;
    for (const auto& [id, execution] : m_executions) {
        if (execution.action == invocation.action && execution.state == ActionExecutionState::Running) {
            if (!running || id < running) running = id;
            runningExecutions.push_back(id);
        }
    }
    const ActionReentryPolicy reentry = options.reentryPolicy.value_or(descriptor->reentryPolicy);
    if (running && reentry == ActionReentryPolicy::IgnoreWhileRunning)
        return Result<ActionExecutionId>::Success(running);
    if (running && reentry == ActionReentryPolicy::Restart) {
        for (const auto execution : runningExecutions) {
            const Status cancelled = Cancel(execution);
            if (!cancelled)
                return Result<ActionExecutionId>::Failure(cancelled.Diagnostics());
        }
    }

    auto provider = FindProvider(descriptor->providerId);
    if ((!provider || provider->Origin() != descriptor->origin ||
         !provider->CanInvoke(invocation.action))) {
        provider.reset();
        for (const auto& [_, candidate] : m_providers) {
            if (candidate && candidate->Origin() == descriptor->origin &&
                candidate->CanInvoke(invocation.action)) {
                provider = candidate;
                break;
            }
        }
    }
    if (!provider)
        return Result<ActionExecutionId>::Failure(DispatchError("PXUIACTION2024",
            "No runtime provider is available for action: " + invocation.action, &invocation));

    ProviderActionStart started = provider->Start(invocation);
    if (!started.status) {
        for (const auto& diagnostic : started.status.Diagnostics()) diag::Emit(diagnostic);
        return Result<ActionExecutionId>::Failure(started.status.Diagnostics());
    }
    const ActionExecutionId id = m_nextExecution++;
    m_executions.emplace(id, Execution{.id=id,
        .action=invocation.action,
        .provider=std::move(provider),
        .providerHandle=started.handle,
        .state=started.pending ? ActionExecutionState::Running
                               : ActionExecutionState::Completed,
        .invocation=std::move(invocation)});
    return Result<ActionExecutionId>::Success(id);
}

Status ActionDispatcher::Dispatch(ActionInvocation invocation, const ActionDispatchOptions options) {
    const ActionExecutionId nextExecution = m_nextExecution;
    auto started = Start(std::move(invocation), options);
    if (!started) return EmitFailure(started.Diagnostics());
    // IgnoreWhileRunning returns the existing execution. Its original owner may
    // be awaiting it, so a fire-and-forget duplicate must not steal ownership.
    if (started.Value() != nextExecution) return Status::Ok();
    Forget(started.Value());
    return Status::Ok();
}

ActionExecutionState ActionDispatcher::State(const ActionExecutionId execution) const {
    const auto found = m_executions.find(execution);
    return found == m_executions.end() ? ActionExecutionState::Unknown : found->second.state;
}

Status ActionDispatcher::Failure(const ActionExecutionId execution) const {
    const auto found = m_executions.find(execution);
    if (found == m_executions.end())
        return Status::Fail(DispatchError("PXUIACTION2025", "Unknown action execution"));
    return found->second.failure;
}

Status ActionDispatcher::Cancel(const ActionExecutionId execution) {
    const auto found = m_executions.find(execution);
    if (found == m_executions.end())
        return Status::Fail(DispatchError("PXUIACTION2025", "Unknown action execution"));
    if (found->second.state == ActionExecutionState::Running && found->second.provider)
        found->second.provider->Cancel(found->second.providerHandle);
    found->second.state = ActionExecutionState::Cancelled;
    return Status::Ok();
}

void ActionDispatcher::CancelAll() {
    for (auto& [_, execution] : m_executions) {
        if (execution.state == ActionExecutionState::Running && execution.provider)
            execution.provider->Cancel(execution.providerHandle);
    }
    m_executions.clear();
}

void ActionDispatcher::CancelSource(const std::string_view sourceScene) {
    if (sourceScene.empty()) return;
    for (auto iterator = m_executions.begin(); iterator != m_executions.end();) {
        auto& execution = iterator->second;
        if (execution.invocation.context.sourceScene != sourceScene) {
            ++iterator;
            continue;
        }
        if (execution.state == ActionExecutionState::Running && execution.provider)
            execution.provider->Cancel(execution.providerHandle);
        iterator = m_executions.erase(iterator);
    }
}

void ActionDispatcher::Forget(const ActionExecutionId execution) {
    const auto found = m_executions.find(execution);
    if (found == m_executions.end()) return;
    if (found->second.state == ActionExecutionState::Running) {
        found->second.autoForget = true;
        return;
    }
    m_executions.erase(found);
}

void ActionDispatcher::Update(const float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) return;
    for (const auto& [_, provider] : m_providers)
        if (provider) provider->Update(deltaSeconds);
    for (auto& [_, execution] : m_executions) {
        if (execution.state != ActionExecutionState::Running || !execution.provider) continue;
        execution.state = execution.provider->Poll(execution.providerHandle);
        if (execution.state == ActionExecutionState::Failed)
            execution.failure = Status::Fail(DispatchError(
                "PXUIACTION2026", "Action provider execution failed: " + execution.action));
    }
    std::erase_if(m_executions,[](const auto& item){return item.second.autoForget&&
        item.second.state!=ActionExecutionState::Running;});
}

std::vector<ActionExecutionCheckpoint> ActionDispatcher::CaptureState() const {
    std::vector<ActionExecutionCheckpoint> state;
    for (const auto& [id, execution] : m_executions) {
        if (execution.state != ActionExecutionState::Running || !execution.provider) continue;
        state.push_back({.execution=id,
                         .invocation=execution.invocation,
                         .providerId=std::string(execution.provider->ProviderId()),
                         .providerHandle=execution.providerHandle,
                         .autoForget=execution.autoForget});
    }
    return state;
}

Status ActionDispatcher::RestoreState(const std::vector<ActionExecutionCheckpoint>& state) {
    std::unordered_map<ActionExecutionId, Execution> restored;
    std::unordered_set<ActionExecutionId> ids;
    ActionExecutionId next = 1;
    for (const auto& checkpoint : state) {
        if (!checkpoint.execution || checkpoint.invocation.action.empty() ||
            !ids.insert(checkpoint.execution).second) {
            return Status::Fail(DispatchError("PXUIACTION2027",
                "Action checkpoint has an invalid or duplicate execution id",
                &checkpoint.invocation));
        }
        const auto* descriptor = m_catalog.Find(checkpoint.invocation.action);
        auto provider = FindProvider(checkpoint.providerId);
        if (!descriptor || !provider || !provider->CanInvoke(checkpoint.invocation.action)) {
            return Status::Fail(DispatchError("PXUIACTION2028",
                "Action checkpoint provider is unavailable: " + checkpoint.invocation.action,
                &checkpoint.invocation));
        }
        const auto providerState = provider->Poll(checkpoint.providerHandle);
        if (providerState != ActionExecutionState::Running &&
            providerState != ActionExecutionState::Completed) {
            return Status::Fail(DispatchError("PXUIACTION2029",
                "Action provider checkpoint could not be restored: " + checkpoint.invocation.action,
                &checkpoint.invocation));
        }
        restored.emplace(checkpoint.execution, Execution{
            .id=checkpoint.execution,
            .action=checkpoint.invocation.action,
            .provider=std::move(provider),
            .providerHandle=checkpoint.providerHandle,
            .state=providerState,
            .autoForget=checkpoint.autoForget,
            .invocation=checkpoint.invocation});
        next = std::max(next, checkpoint.execution + 1);
    }
    m_executions = std::move(restored);
    m_nextExecution = next;
    return Status::Ok();
}

}  // namespace px::ui
