#include "Engine/UI/Styles/StyleResolver.h"

#include <algorithm>
#include <bit>
#include <set>
#include <tuple>

namespace px::ui {
namespace {

diag::Diagnostic StyleError(std::string code, std::string message, std::string details = {},
                            std::string property = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "UI.Style",
                                .message = std::move(message),
                                .details = std::move(details)};
    diagnostic.source.property = std::move(property);
    return diagnostic;
}

int SelectorPriority(StyleStateMask selector) {
    int result = 0;
    for (StyleState state : {StyleState::Checked, StyleState::Selected, StyleState::Focused,
                             StyleState::Hover, StyleState::Pressed, StyleState::Disabled}) {
        if ((selector & StateMask(state)) != 0)
            result = std::max(result, StyleStatePriority(state));
    }
    return result;
}

std::string SelectorName(StyleStateMask selector) {
    if (selector == 0) return StyleStateName(StyleState::Normal);
    std::string result;
    for (StyleState state : {StyleState::Checked, StyleState::Selected, StyleState::Focused,
                             StyleState::Hover, StyleState::Pressed, StyleState::Disabled}) {
        if ((selector & StateMask(state)) == 0) continue;
        if (!result.empty()) result += " + ";
        result += StyleStateName(state);
    }
    return result;
}

struct TokenResolution {
    Variant value;
    std::vector<TokenId> chain;
};

Result<TokenResolution> ResolveTokenRecursive(
    const StyleThemeData& theme, const TokenId& token, std::vector<TokenId>& stack,
    ResolvedStyleDependencies* dependencies) {
    const auto* definition = theme.FindToken(token);
    if (!definition)
        return Result<TokenResolution>::Failure(StyleError(
            "PXSTYLE3301", "Theme token does not exist", token.ToString()));
    if (std::find(stack.begin(), stack.end(), token) != stack.end()) {
        std::string cycle;
        for (const auto& item : stack) {
            if (!cycle.empty()) cycle += " -> ";
            const auto* current = theme.FindToken(item);
            cycle += current ? current->displayName : item.ToString();
        }
        cycle += " -> " + definition->displayName;
        return Result<TokenResolution>::Failure(
            StyleError("PXSTYLE3302", "Theme token cycle detected", cycle));
    }
    if (dependencies) dependencies->tokens.insert(token);
    stack.push_back(token);

    TokenResolution resolved;
    resolved.chain.push_back(token);
    if (definition->value.IsUnset()) {
        stack.pop_back();
        return Result<TokenResolution>::Failure(StyleError(
            "PXSTYLE3303", "Theme token has no value", definition->displayName));
    }
    if (definition->value.IsLiteral()) {
        resolved.value = definition->value.LiteralValue().Clone();
    } else {
        auto nested = ResolveTokenRecursive(theme, definition->value.TokenReference(), stack,
                                            dependencies);
        if (!nested) {
            stack.pop_back();
            return nested;
        }
        resolved.value = nested.Value().value.Clone();
        resolved.chain.insert(resolved.chain.end(), nested.Value().chain.begin(),
                              nested.Value().chain.end());
    }
    stack.pop_back();

    if (definition->type != VariantType::Null &&
        !IsStyleValueTypeCompatible(definition->type, resolved.value.Type())) {
        return Result<TokenResolution>::Failure(StyleError(
            "PXSTYLE3304", "Theme token type mismatch",
            definition->displayName + " expects " + ToString(definition->type) + " but resolved " +
                ToString(resolved.value.Type())));
    }
    if (definition->type != VariantType::Null)
        resolved.value = CoerceStyleValue(std::move(resolved.value), definition->type);
    return Result<TokenResolution>::Success(std::move(resolved));
}

struct StateLayer {
    const StyleBlock* block = nullptr;
    StyleSourceTraceEntry source;
    std::size_t order = 0;
};

struct StateCandidate {
    const StylePropertyMap* properties = nullptr;
    StyleSourceTraceEntry source;
    int priority = 0;
    int specificity = 0;
    StyleStateMask selector = 0;
    std::size_t layerOrder = 0;
};

}  // namespace

const ResolvedStyleProperty* ResolvedStyle::Find(std::string_view property) const {
    const auto found = properties.find(std::string(property));
    return found == properties.end() ? nullptr : &found->second;
}

Result<Variant> StyleResolver::ResolveTokenValue(
    const StyleThemeData& theme, const TokenId& token, std::optional<VariantType> expectedType,
    ResolvedStyleDependencies* dependencies, std::vector<TokenId>* trace) const {
    std::vector<TokenId> stack;
    auto resolved = ResolveTokenRecursive(theme, token, stack, dependencies);
    if (!resolved) return Result<Variant>::Failure(resolved.Diagnostics());
    if (expectedType && !IsStyleValueTypeCompatible(*expectedType, resolved.Value().value.Type())) {
        const auto* definition = theme.FindToken(token);
        return Result<Variant>::Failure(StyleError(
            "PXSTYLE3305", "Token is incompatible with the style property",
            std::string(definition ? definition->displayName : token.ToString()) + " resolves " +
                ToString(resolved.Value().value.Type()) + ", expected " + ToString(*expectedType)));
    }
    if (trace) *trace = resolved.Value().chain;
    Variant value = resolved.Value().value.Clone();
    if (expectedType) value = CoerceStyleValue(std::move(value), *expectedType);
    return Result<Variant>::Success(std::move(value));
}

Result<ResolvedStyle> StyleResolver::Resolve(const StyleThemeData& theme,
                                             const StyleResolveRequest& request,
                                             const StylePropertyRegistry& registry) const {
    ResolvedStyle output;
    output.themeRevision = theme.revision;
    output.propertyRegistryRevision = registry.Revision();
    std::vector<diag::Diagnostic> diagnostics;
    std::vector<StateLayer> stateLayers;
    std::size_t stackOrder = 0;

    const auto apply = [&](const StylePropertyMap& values,
                           StyleSourceTraceEntry source) -> void {
        std::vector<std::string> names;
        names.reserve(values.size());
        for (const auto& [name, value] : values) {
            (void)value;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            const StyleValue& styleValue = values.at(name);
            if (styleValue.IsUnset()) continue;
            const auto* descriptor = registry.Find(name);
            if (!descriptor) {
                diagnostics.push_back(StyleError("PXSTYLE3310", "Unsupported style property",
                                                 name, name));
                continue;
            }
            if (!registry.Supports(name, request.controlType)) {
                diagnostics.push_back(StyleError(
                    "PXSTYLE3311", "Style property is unsupported by this Control type",
                    request.controlType + ": " + name, name));
                continue;
            }

            Variant resolved;
            std::vector<TokenId> tokenChain;
            if (styleValue.IsLiteral()) {
                resolved = styleValue.LiteralValue().Clone();
                if (!IsStyleValueTypeCompatible(descriptor->valueType, resolved.Type())) {
                    diagnostics.push_back(StyleError(
                        "PXSTYLE3312", "Style property type mismatch",
                        name + " expects " + ToString(descriptor->valueType) + " but received " +
                            ToString(resolved.Type()),
                        name));
                    continue;
                }
                resolved = CoerceStyleValue(std::move(resolved), descriptor->valueType);
            } else {
                auto token = ResolveTokenValue(theme, styleValue.TokenReference(),
                                               descriptor->valueType, &output.dependencies,
                                               &tokenChain);
                if (!token) {
                    for (auto diagnostic : token.Diagnostics()) {
                        diagnostic.source.property = name;
                        diagnostics.push_back(std::move(diagnostic));
                    }
                    continue;
                }
                resolved = token.TakeValue();
            }

            ResolvedStyleProperty next;
            next.value = std::move(resolved);
            next.source = source;
            next.tokenChain = std::move(tokenChain);
            if (const auto found = output.properties.find(name); found != output.properties.end()) {
                next.overriddenSources = found->second.overriddenSources;
                next.overriddenSources.push_back(found->second.source);
            }
            output.properties.insert_or_assign(name, std::move(next));
        }
    };

    const auto addBlock = [&](const StyleBlock& block, StyleSourceTraceEntry source) {
        source.stackOrder = stackOrder;
        apply(block.properties, source);
        stateLayers.push_back({&block, source, stackOrder});
        ++stackOrder;
    };

    addBlock(theme.globalDefaults,
             {.layer = StyleLayerKind::ThemeGlobalDefaults,
              .label = StyleLayerName(StyleLayerKind::ThemeGlobalDefaults)});

    if (const auto control = theme.controlTypeDefaults.find("Control");
        control != theme.controlTypeDefaults.end())
        addBlock(control->second,
                 {.layer = StyleLayerKind::ControlTypeDefaults, .label = "Control"});
    if (request.controlType != "Control") {
        if (const auto exact = theme.controlTypeDefaults.find(request.controlType);
            exact != theme.controlTypeDefaults.end())
            addBlock(exact->second,
                     {.layer = StyleLayerKind::ControlTypeDefaults,
                      .label = request.controlType});
    }

    if (request.binding.baseStyle) {
        const auto* style = theme.FindStyle(*request.binding.baseStyle);
        if (!style) {
            diagnostics.push_back(StyleError("PXSTYLE3313", "Base style does not exist",
                                             request.binding.baseStyle->ToString()));
        } else if (!IsStyleCompatibleWith(style->compatibleTypes, request.controlType)) {
            diagnostics.push_back(StyleError("PXSTYLE3314",
                                             "Base style is incompatible with Control type",
                                             style->displayName + " / " + request.controlType));
        } else {
            output.dependencies.styles.insert(style->id);
            addBlock(*style, {.layer = StyleLayerKind::BaseStyle,
                              .label = style->displayName,
                              .style = style->id});
        }
    }

    std::unordered_set<VariantAxisId, UuidHash> handledAxes;
    for (const auto& axis : theme.variantAxes) {
        const auto selection = request.binding.variants.find(axis.id);
        if (!IsStyleCompatibleWith(axis.compatibleTypes, request.controlType)) {
            if (selection != request.binding.variants.end())
                diagnostics.push_back(StyleError(
                    "PXSTYLE3315", "Variant axis is incompatible with Control type",
                    axis.displayName + " / " + request.controlType));
            continue;
        }
        handledAxes.insert(axis.id);
        const VariantValueId selected =
            selection == request.binding.variants.end() ? axis.defaultValue : selection->second;
        if (selected.Empty()) continue;
        const auto* value = axis.FindValue(selected);
        if (!value) {
            diagnostics.push_back(StyleError("PXSTYLE3316", "Variant axis value does not exist",
                                             axis.displayName + " / " + selected.ToString()));
            continue;
        }
        output.dependencies.variantAxes.insert(axis.id);
        addBlock(*value, {.layer = StyleLayerKind::VariantAxis,
                          .label = axis.displayName + " / " + value->displayName,
                          .variantAxis = axis.id,
                          .variantValue = value->id});
    }
    for (const auto& [axis, value] : request.binding.variants) {
        if (!handledAxes.contains(axis) && !theme.FindAxis(axis))
            diagnostics.push_back(StyleError("PXSTYLE3317", "Variant axis does not exist",
                                             axis.ToString() + " / " + value.ToString()));
    }

    for (const auto& styleId : request.binding.appliedStyles) {
        const auto* style = theme.FindStyle(styleId);
        if (!style) {
            diagnostics.push_back(StyleError("PXSTYLE3318", "Applied style does not exist",
                                             styleId.ToString()));
            continue;
        }
        if (!IsStyleCompatibleWith(style->compatibleTypes, request.controlType)) {
            diagnostics.push_back(StyleError("PXSTYLE3319",
                                             "Applied style is incompatible with Control type",
                                             style->displayName + " / " + request.controlType));
            continue;
        }
        output.dependencies.styles.insert(style->id);
        addBlock(*style, {.layer = StyleLayerKind::AppliedStyle,
                          .label = style->displayName,
                          .style = style->id});
    }

    std::vector<StateCandidate> candidates;
    for (const auto& layer : stateLayers) {
        for (const auto& [selector, properties] : layer.block->stateOverrides) {
            if (selector == 0 || !request.activeStates.ContainsAll(selector)) continue;
            StyleSourceTraceEntry source = layer.source;
            source.layer = StyleLayerKind::ActiveState;
            source.label += " / " + SelectorName(selector);
            source.stateSelector = selector;
            candidates.push_back({&properties, std::move(source), SelectorPriority(selector),
                                  static_cast<int>(std::popcount(selector)), selector,
                                  layer.order});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.priority, lhs.specificity, lhs.selector, lhs.layerOrder) <
               std::tie(rhs.priority, rhs.specificity, rhs.selector, rhs.layerOrder);
    });
    for (auto& candidate : candidates) {
        candidate.source.stackOrder = stackOrder++;
        apply(*candidate.properties, candidate.source);
    }

    apply(request.binding.componentOverrides,
          {.layer = StyleLayerKind::ComponentInstanceOverride,
           .label = StyleLayerName(StyleLayerKind::ComponentInstanceOverride),
           .stackOrder = stackOrder++});
    apply(request.binding.localOverrides,
          {.layer = StyleLayerKind::LocalControlOverride,
           .label = StyleLayerName(StyleLayerKind::LocalControlOverride),
           .stackOrder = stackOrder++});

    if (!diagnostics.empty()) return Result<ResolvedStyle>::Failure(std::move(diagnostics));
    return Result<ResolvedStyle>::Success(std::move(output));
}

Status StyleResolver::ValidateTheme(const StyleThemeData& theme,
                                    const StylePropertyRegistry& registry) const {
    std::vector<diag::Diagnostic> diagnostics;
    std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
    const auto collect = [&](const auto& result) {
        if (result) return;
        for (const auto& diagnostic : result.Diagnostics()) {
            const auto key = std::make_tuple(diagnostic.code, diagnostic.message,
                                             diagnostic.details,
                                             diagnostic.source.property);
            if (seen.insert(key).second) diagnostics.push_back(diagnostic);
        }
    };

    for (const auto& token : theme.tokens)
        collect(ResolveTokenValue(theme, token.id, token.type == VariantType::Null
                                                       ? std::nullopt
                                                       : std::optional(token.type)));

    std::set<ControlTypeId> controlTypes{"Control"};
    for (const auto& [type, block] : theme.controlTypeDefaults) {
        (void)block;
        controlTypes.insert(type);
    }
    for (const auto& style : theme.styles)
        controlTypes.insert(style.compatibleTypes.begin(), style.compatibleTypes.end());
    for (const auto& axis : theme.variantAxes)
        controlTypes.insert(axis.compatibleTypes.begin(), axis.compatibleTypes.end());
    controlTypes.erase("*");

    for (const auto& type : controlTypes) {
        StyleResolveRequest base{.controlType = type};
        collect(Resolve(theme, base, registry));
        for (const auto& style : theme.styles) {
            if (!IsStyleCompatibleWith(style.compatibleTypes, type)) continue;
            StyleResolveRequest request{.controlType = type};
            request.binding.baseStyle = style.id;
            collect(Resolve(theme, request, registry));
            for (StyleState state : {StyleState::Checked, StyleState::Selected,
                                     StyleState::Focused, StyleState::Hover,
                                     StyleState::Pressed, StyleState::Disabled}) {
                request.activeStates = StyleStateSet(state);
                collect(Resolve(theme, request, registry));
            }
        }
        for (const auto& axis : theme.variantAxes) {
            if (!IsStyleCompatibleWith(axis.compatibleTypes, type)) continue;
            for (const auto& value : axis.values) {
                StyleResolveRequest request{.controlType = type};
                request.binding.variants[axis.id] = value.id;
                collect(Resolve(theme, request, registry));
            }
        }
    }
    return diagnostics.empty() ? Status::Ok() : Status::Fail(std::move(diagnostics));
}

}  // namespace px::ui
