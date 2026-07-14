#include "Engine/UI/Actions/TriggerBinding.h"

#include <unordered_set>

namespace px::ui {
namespace {

diag::Diagnostic BindingError(std::string code, std::string message, const Uuid& node,
                              const std::string& signal, const std::string& sourceScene) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "UI.Action",
                                .message = std::move(message)};
    diagnostic.source.path = sourceScene;
    diagnostic.source.nodeId = node.Empty() ? std::string{} : node.ToString();
    diagnostic.source.property = "triggers." + signal;
    return diagnostic;
}

}  // namespace

const char* TriggerBindingKindName(const TriggerBindingKind kind) {
    return kind == TriggerBindingKind::Flow ? "flow" : "action";
}

ActionInvocation TriggerBinding::Invocation() const {
    ActionInvocation invocation;
    invocation.action = action;
    for (const auto& [name, value] : arguments) invocation.arguments[name] = value.Clone();
    invocation.context.sourceScene = sourceScene;
    invocation.context.sourceNode = node;
    invocation.context.signal = signal;
    return invocation;
}

Status TriggerBinding::Resolve(const ActionCatalog& catalog) {
    resolved = false;
    resolutionMessage.clear();
    if (kind == TriggerBindingKind::Flow) {
        if (graphEntry.Empty()) {
            resolutionMessage = "Behavior Graph binding has no entry UUID";
            return Status::Fail(BindingError("PXUIACTION2045", resolutionMessage, node, signal,
                                             sourceScene));
        }
        resolved = true;
        return Status::Ok();
    }
    const auto* descriptor = catalog.Find(action);
    if (!descriptor) {
        resolutionMessage = "Missing action: " + action;
        return Status::Fail(BindingError("PXUIACTION2010", resolutionMessage, node, signal,
                                         sourceScene));
    }
    if (!descriptor->available) {
        resolutionMessage = descriptor->unavailableReason.empty()
                              ? "Action is unavailable: " + action
                              : descriptor->unavailableReason;
        return Status::Fail(BindingError("PXUIACTION2011", resolutionMessage, node, signal,
                                         sourceScene));
    }
    auto normalized = catalog.ValidateAndNormalize(Invocation());
    if (!normalized) {
        resolutionMessage = normalized.Diagnostics().empty()
                              ? "Invalid action arguments" : normalized.Diagnostics().front().message;
        return Status::Fail(normalized.Diagnostics());
    }
    arguments = normalized.TakeValue();
    resolved = true;
    return Status::Ok();
}

Variant TriggerBinding::ToVariant() const {
    VariantObject result{{"kind", std::string(TriggerBindingKindName(kind))},
                         {"reentry", std::string(ActionReentryPolicyName(reentry))}};
    if (kind == TriggerBindingKind::Flow) {
        result["entry"] = graphEntry;
        return Variant(std::move(result));
    }
    VariantObject copied;
    for (const auto& [name, value] : arguments) copied[name] = value.Clone();
    result["action"] = action;
    result["arguments"] = std::move(copied);
    return Variant(std::move(result));
}

Result<TriggerBinding> ParseTriggerBinding(const Uuid& node, std::string signal,
                                           const Variant& value, const std::string& sourceScene) {
    const auto* definition = value.AsObject();
    if (!definition)
        return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2040",
            "Trigger binding must be a typed object", node, signal, sourceScene));
    const auto kindIt = definition->find("kind");
    const auto* kindName = kindIt == definition->end()
                             ? nullptr : kindIt->second.TryGet<std::string>();
    if (!kindName || (*kindName != "action" && *kindName != "flow"))
        return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2043",
            "Trigger binding kind must be 'action' or 'flow'", node, signal, sourceScene));
    TriggerBinding binding;
    binding.node = node;
    binding.signal = std::move(signal);
    binding.kind = *kindName == "flow" ? TriggerBindingKind::Flow
                                        : TriggerBindingKind::Action;
    binding.sourceScene = sourceScene;
    const auto reentryIt=definition->find("reentry");
    const auto* reentryName=reentryIt==definition->end()?nullptr:
        reentryIt->second.TryGet<std::string>();
    const auto reentry=reentryName?ParseActionReentryPolicy(*reentryName):std::nullopt;
    if(!reentry)return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2044",
        "Trigger binding requires a valid reentry policy",node,binding.signal,sourceScene));
    binding.reentry=*reentry;
    if (binding.kind == TriggerBindingKind::Flow) {
        const std::unordered_set<std::string_view> allowed{"kind","entry","reentry"};
        for(const auto& [key,_]:*definition)if(!allowed.contains(key))
            return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2046",
                "Flow binding contains unsupported field: "+key,node,binding.signal,sourceScene));
        const auto entryIt = definition->find("entry");
        const auto* entry = entryIt == definition->end() ? nullptr : entryIt->second.TryGet<Uuid>();
        if (!entry || entry->Empty())
            return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2045",
                "Flow binding requires an entry UUID", node, binding.signal, sourceScene));
        binding.graphEntry = *entry;
        return Result<TriggerBinding>::Success(std::move(binding));
    }
    const std::unordered_set<std::string_view> allowed{"kind","action","arguments","reentry"};
    for(const auto& [key,_]:*definition)if(!allowed.contains(key))
        return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2046",
            "Direct Action binding contains unsupported field: "+key,node,binding.signal,sourceScene));
    const auto actionIt = definition->find("action");
    const auto* action = actionIt == definition->end()
                           ? nullptr : actionIt->second.TryGet<std::string>();
    if (!action || action->empty())
        return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2041",
            "Trigger binding requires a stable action id", node, signal, sourceScene));
    VariantObject arguments;
    const auto argumentsIt=definition->find("arguments");
    const auto* object=argumentsIt==definition->end()?nullptr:argumentsIt->second.AsObject();
    if(!object)return Result<TriggerBinding>::Failure(BindingError("PXUIACTION2042",
        "Direct Action binding requires typed arguments",node,signal,sourceScene));
    for(const auto& [name,argument]:*object)arguments[name]=argument.Clone();
    binding.action = *action;
    binding.arguments = std::move(arguments);
    return Result<TriggerBinding>::Success(std::move(binding));
}

}  // namespace px::ui
