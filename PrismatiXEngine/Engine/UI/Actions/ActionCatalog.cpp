#include "Engine/UI/Actions/ActionCatalog.h"

#include "Engine/UI/Actions/BuiltInActionProvider.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace px::ui {
namespace {

diag::Diagnostic CatalogError(std::string code, std::string message,
                              const ActionContext* context = nullptr,
                              std::string property = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "UI.Action",
                                .message = std::move(message)};
    if (context) {
        diagnostic.source.path = context->sourceScene;
        diagnostic.source.nodeId = context->sourceNode.Empty() ? std::string{} : context->sourceNode.ToString();
        diagnostic.source.property = property.empty() ? context->signal : std::move(property);
    }
    return diagnostic;
}

std::string Lower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool ContainsInsensitive(std::string_view value, std::string_view query) {
    return query.empty() || Lower(value).find(Lower(query)) != std::string::npos;
}

bool NumericValue(const Variant& value, double& result) {
    if (const auto* number = value.TryGet<double>()) { result = *number; return true; }
    if (const auto* integer = value.TryGet<std::int64_t>()) {
        result = static_cast<double>(*integer); return true;
    }
    return false;
}

}  // namespace

Status ActionCatalog::NormalizeAndValidateDescriptor(ActionDescriptor& descriptor) {
    if (descriptor.id.empty())
        return Status::Fail(CatalogError("PXUIACTION2001", "Action id is empty"));
    if (descriptor.displayName.empty()) descriptor.displayName = descriptor.label;
    if (descriptor.label.empty()) descriptor.label = descriptor.displayName;
    if (descriptor.displayName.empty()) descriptor.displayName = descriptor.id;
    if (descriptor.label.empty()) descriptor.label = descriptor.id;
    if (descriptor.category.empty()) descriptor.category = "Other";
    if (descriptor.providerId.empty()) {
        descriptor.providerId = descriptor.origin == ActionOrigin::BuiltIn ? "builtin" :
                                descriptor.origin == ActionOrigin::Plugin ? "plugin" : "script";
    }
    std::unordered_set<std::string> names;
    for (auto& argument : descriptor.arguments) {
        if (argument.name.empty() || !names.insert(argument.name).second)
            return Status::Fail(CatalogError("PXUIACTION2003",
                "Action has an empty or duplicate argument: " + descriptor.id));
        if (argument.displayName.empty()) argument.displayName = argument.name;
        if (argument.type == VariantType::Null)
            return Status::Fail(CatalogError("PXUIACTION2004",
                "Action argument has no typed value: " + descriptor.id + "." + argument.name));
        if (argument.minimum && argument.maximum && *argument.minimum > *argument.maximum)
            return Status::Fail(CatalogError("PXUIACTION2005",
                "Action argument range is invalid: " + descriptor.id + "." + argument.name));
        if (argument.defaultValue && argument.defaultValue->Type() != argument.type)
            return Status::Fail(CatalogError("PXUIACTION2006",
                "Action argument default type is invalid: " + descriptor.id + "." + argument.name));
    }
    return Status::Ok();
}

Status ActionCatalog::Register(ActionDescriptor descriptor) {
    const Status valid = NormalizeAndValidateDescriptor(descriptor);
    if (!valid) return valid;
    if (m_byId.contains(descriptor.id))
        return Status::Fail(CatalogError("PXUIACTION2002", "Duplicate action: " + descriptor.id));
    m_byId[descriptor.id] = m_descriptors.size();
    m_descriptors.push_back(std::move(descriptor));
    return Status::Ok();
}

Status ActionCatalog::ReplaceSource(const ActionOrigin origin, const std::string_view sourceId,
                                    std::vector<ActionDescriptor> descriptors) {
    std::unordered_set<std::string> incoming;
    for (auto& descriptor : descriptors) {
        descriptor.origin = origin;
        descriptor.sourceId = std::string(sourceId);
        const Status valid = NormalizeAndValidateDescriptor(descriptor);
        if (!valid) return valid;
        if (!incoming.insert(descriptor.id).second)
            return Status::Fail(CatalogError("PXUIACTION2002", "Duplicate action: " + descriptor.id));
        if (const auto* current = Find(descriptor.id);
            current && !(current->origin == origin && current->sourceId == sourceId))
            return Status::Fail(CatalogError("PXUIACTION2002", "Duplicate action: " + descriptor.id));
    }

    std::vector<ActionDescriptor> next;
    next.reserve(m_descriptors.size() + descriptors.size());
    for (auto& descriptor : m_descriptors)
        if (!(descriptor.origin == origin && descriptor.sourceId == sourceId))
            next.push_back(std::move(descriptor));
    for (auto& descriptor : descriptors) next.push_back(std::move(descriptor));
    m_descriptors = std::move(next);
    Reindex();
    return Status::Ok();
}

Status ActionCatalog::RemoveSource(const ActionOrigin origin, const std::string_view sourceId) {
    std::erase_if(m_descriptors, [&](const ActionDescriptor& descriptor) {
        return descriptor.origin == origin && descriptor.sourceId == sourceId;
    });
    Reindex();
    return Status::Ok();
}

void ActionCatalog::Reindex() {
    m_byId.clear();
    for (std::size_t index = 0; index < m_descriptors.size(); ++index)
        m_byId[m_descriptors[index].id] = index;
}

const ActionDescriptor* ActionCatalog::Find(const std::string_view id) const {
    const auto found = m_byId.find(std::string(id));
    return found == m_byId.end() ? nullptr : &m_descriptors[found->second];
}

std::vector<ActionDescriptor> ActionCatalog::Search(const std::string_view text,
                                                    const std::string_view category,
                                                    const std::optional<ActionOrigin> origin,
                                                    const bool includeUnavailable) const {
    std::vector<ActionDescriptor> result;
    for (const auto& descriptor : m_descriptors) {
        if (origin && descriptor.origin != *origin) continue;
        if (!category.empty() && descriptor.category != category) continue;
        if (!includeUnavailable && !descriptor.available) continue;
        if (!ContainsInsensitive(descriptor.id, text) &&
            !ContainsInsensitive(descriptor.displayName, text) &&
            !ContainsInsensitive(descriptor.description, text) &&
            !ContainsInsensitive(descriptor.category, text)) continue;
        result.push_back(descriptor);
    }
    std::sort(result.begin(), result.end(), [](const ActionDescriptor& left,
                                               const ActionDescriptor& right) {
        return std::tie(left.category, left.displayName, left.id) <
               std::tie(right.category, right.displayName, right.id);
    });
    return result;
}

std::vector<std::string> ActionCatalog::Categories(const std::optional<ActionOrigin> origin) const {
    std::vector<std::string> result;
    for (const auto& descriptor : m_descriptors) {
        if (origin && descriptor.origin != *origin) continue;
        if (std::find(result.begin(), result.end(), descriptor.category) == result.end())
            result.push_back(descriptor.category);
    }
    std::sort(result.begin(), result.end());
    return result;
}

Result<VariantObject> ActionCatalog::ValidateAndNormalize(
    const ActionInvocation& invocation) const {
    const auto* descriptor = Find(invocation.action);
    if (!descriptor)
        return Result<VariantObject>::Failure(CatalogError("PXUIACTION2010",
            "Missing action: " + invocation.action, &invocation.context, "triggers." + invocation.context.signal));
    if (!descriptor->available)
        return Result<VariantObject>::Failure(CatalogError("PXUIACTION2011",
            "Action is unavailable: " + invocation.action,
            &invocation.context, "triggers." + invocation.context.signal));

    VariantObject normalized;
    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& argument : descriptor->arguments) {
        const auto found = invocation.arguments.find(argument.name);
        if (found == invocation.arguments.end()) {
            if (argument.defaultValue) normalized[argument.name] = argument.defaultValue->Clone();
            else if (argument.required)
                diagnostics.push_back(CatalogError("PXUIACTION2012",
                    "Missing required action argument: " + invocation.action + "." + argument.name,
                    &invocation.context, "triggers." + invocation.context.signal + ".arguments." + argument.name));
            continue;
        }
        Variant value = found->second.Clone();
        if (argument.type == VariantType::Number && value.Type() == VariantType::Integer)
            value = Variant(static_cast<double>(*value.TryGet<std::int64_t>()));
        if (value.Type() != argument.type) {
            diagnostics.push_back(CatalogError("PXUIACTION2013",
                "Wrong action argument type for " + invocation.action + "." + argument.name +
                ": expected " + ToString(argument.type) + ", got " + ToString(value.Type()),
                &invocation.context, "triggers." + invocation.context.signal + ".arguments." + argument.name));
            continue;
        }
        if (!argument.enumValues.empty()) {
            const auto* choice = value.TryGet<std::string>();
            if (!choice || std::find(argument.enumValues.begin(), argument.enumValues.end(), *choice) ==
                               argument.enumValues.end()) {
                diagnostics.push_back(CatalogError("PXUIACTION2014",
                    "Invalid enum value for " + invocation.action + "." + argument.name,
                    &invocation.context, "triggers." + invocation.context.signal + ".arguments." + argument.name));
                continue;
            }
        }
        double number = 0.0;
        if ((argument.minimum || argument.maximum) && NumericValue(value, number)) {
            if ((argument.minimum && number < *argument.minimum) ||
                (argument.maximum && number > *argument.maximum)) {
                diagnostics.push_back(CatalogError("PXUIACTION2015",
                    "Action argument is outside its allowed range: " + invocation.action + "." + argument.name,
                    &invocation.context, "triggers." + invocation.context.signal + ".arguments." + argument.name));
                continue;
            }
        }
        normalized[argument.name] = std::move(value);
    }
    if (!descriptor->allowAdditionalArguments) {
        for (const auto& [name, _] : invocation.arguments) {
            if (std::none_of(descriptor->arguments.begin(), descriptor->arguments.end(),
                [&](const ActionArgumentDescriptor& argument) { return argument.name == name; }))
                diagnostics.push_back(CatalogError("PXUIACTION2016",
                    "Unknown action argument: " + invocation.action + "." + name,
                    &invocation.context, "triggers." + invocation.context.signal + ".arguments." + name));
        }
    } else {
        for (const auto& [name, value] : invocation.arguments)
            if (!normalized.contains(name)) normalized[name] = value.Clone();
    }
    return diagnostics.empty() ? Result<VariantObject>::Success(std::move(normalized))
                               : Result<VariantObject>::Failure(std::move(diagnostics));
}

ActionCatalog& ActionCatalog::Global() {
    static ActionCatalog catalog = [] {
        ActionCatalog value;
        (void)RegisterBuiltInActionDescriptors(value);
        return value;
    }();
    return catalog;
}

}  // namespace px::ui
