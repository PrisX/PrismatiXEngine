#include "Editor/Tools/UIDesigner/DesignerDiagnostics.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace px::editor {
namespace {

bool Compatible(VariantType target, VariantType source) {
    return target == source || (target == VariantType::Number && source == VariantType::Integer);
}

std::string ExpectedResourceType(const PropertyInfo& property) {
    if (!property.editor.resourceFilter.empty()) return property.editor.resourceFilter;
    return HasFlag(property.flags, PropertyFlags::ResourcePath) ? "resource" : std::string{};
}

}  // namespace

diag::Diagnostic DesignerDiagnostics::Make(diag::Severity severity, std::string code,
                                           std::string message,
                                           const UISceneDocument& document, const Uuid& node,
                                           std::string property, std::string details) {
    diag::Diagnostic result{.severity = severity,
                            .code = std::move(code),
                            .category = "Editor.UIDesigner.Problems",
                            .message = std::move(message),
                            .details = std::move(details),
                            .source = {},
                            .operationId = {},
                            .quickFix = {}};
    result.source.resourceId = document.DocumentId().ToString();
    result.source.path = document.Path().generic_string();
    if (!node.Empty()) result.source.nodeId = node.ToString();
    result.source.property = std::move(property);
    return result;
}

void DesignerDiagnostics::Clear() {
    m_items.clear();
    m_byNode.clear();
}

void DesignerDiagnostics::Add(diag::Diagnostic diagnostic) {
    const auto duplicate = std::find_if(
        m_items.begin(), m_items.end(), [&](const auto& item) {
            return item.code == diagnostic.code && item.source.path == diagnostic.source.path &&
                   item.source.nodeId == diagnostic.source.nodeId &&
                   item.source.property == diagnostic.source.property &&
                   item.message == diagnostic.message;
        });
    if (duplicate != m_items.end()) return;
    const std::size_t index = m_items.size();
    if (!diagnostic.source.nodeId.empty()) m_byNode.emplace(diagnostic.source.nodeId, index);
    m_items.push_back(std::move(diagnostic));
}

void DesignerDiagnostics::ValidateTokens(const UISceneDocument& document) {
    const VariantObject* tokens = nullptr;
    for (const char* key : {"theme.tokens", "tokens"}) {
        const auto found = document.Data().properties.find(key);
        if (found != document.Data().properties.end() && found->second.AsObject()) {
            tokens = found->second.AsObject();
            break;
        }
    }
    const VariantObject empty;
    if (!tokens) tokens = &empty;

    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> resolved;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        if (resolved.contains(name)) return;
        const auto found = tokens->find(name);
        if (found == tokens->end()) return;
        if (!visiting.insert(name).second) {
            Add(Make(diag::Severity::Error, "PXEDUIP5001", "Theme token cycle detected",
                     document, {}, "tokens." + name, name));
            return;
        }
        if (const auto* reference = found->second.TryGet<TokenRefValue>()) {
            if (!tokens->contains(reference->name)) {
                Add(Make(diag::Severity::Error, "PXEDUIP5002", "Theme token is missing",
                         document, {}, "tokens." + name, reference->name));
            } else {
                visit(reference->name);
            }
        }
        visiting.erase(name);
        resolved.insert(name);
    };
    for (const auto& [name, value] : *tokens) {
        (void)value;
        visit(name);
    }

    for (const auto& node : document.Data().nodes) {
        for (const auto& [propertyName, value] : node.properties) {
            const auto* reference = value.TryGet<TokenRefValue>();
            if (!reference) continue;
            const auto token = tokens->find(reference->name);
            if (token == tokens->end()) {
                Add(Make(diag::Severity::Error, "PXEDUIP5003", "Property references a missing token",
                         document, node.id, propertyName, reference->name));
                continue;
            }
            const auto* descriptor = TypeRegistry::Global().FindProperty(node.type, propertyName);
            if (descriptor && token->second.Type() != VariantType::TokenRef &&
                !Compatible(descriptor->type, token->second.Type())) {
                Add(Make(diag::Severity::Error, "PXEDUIP5004",
                         "Token type is incompatible with the property", document, node.id,
                         propertyName, reference->name));
            }
        }
    }
}

void DesignerDiagnostics::Refresh(const UISceneDocument& document) {
    Refresh(document, ValidationContext{});
}

void DesignerDiagnostics::Refresh(const UISceneDocument& document,
                                  const ValidationContext& context) {
    Clear();
    std::unordered_set<Uuid, UuidHash> ids;
    std::size_t roots = 0;
    for (const auto& node : document.Data().nodes) {
        if (node.id.Empty() || !ids.insert(node.id).second) {
            Add(Make(diag::Severity::Error, "PXEDUIP5010",
                     "UI scene contains an empty or duplicate node UUID", document, node.id));
        }
        if (node.parent.Empty()) ++roots;
    }
    if (roots != 1) {
        Add(Make(diag::Severity::Error, "PXEDUIP5011",
                 "UI scene must contain exactly one root Control", document));
    }

    for (const auto& node : document.Data().nodes) {
        if (!node.parent.Empty() && !ids.contains(node.parent)) {
            Add(Make(diag::Severity::Error, "PXEDUIP5012", "UI node has a missing parent",
                     document, node.id, "$parent"));
        }
        const auto anchorsIt = node.properties.find("anchors");
        if (anchorsIt != node.properties.end()) {
            if (const auto* anchors = anchorsIt->second.TryGet<Rect>()) {
                if (!std::isfinite(anchors->x) || !std::isfinite(anchors->y) ||
                    !std::isfinite(anchors->w) || !std::isfinite(anchors->h) ||
                    anchors->x > anchors->w || anchors->y > anchors->h) {
                    Add(Make(diag::Severity::Error, "PXEDUIP5013",
                             "Control has an impossible anchor rectangle", document, node.id,
                             "anchors"));
                }
            }
        }
        Vec2 minimum{};
        Vec2 maximum{std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max()};
        if (const auto found = node.properties.find("minimumSize");
            found != node.properties.end() && found->second.TryGet<Vec2>()) {
            minimum = *found->second.TryGet<Vec2>();
        }
        if (const auto found = node.properties.find("maximumSize");
            found != node.properties.end() && found->second.TryGet<Vec2>()) {
            maximum = *found->second.TryGet<Vec2>();
        }
        if (minimum.x < 0 || minimum.y < 0 || maximum.x < minimum.x ||
            maximum.y < minimum.y) {
            Add(Make(diag::Severity::Error, "PXEDUIP5014",
                     "Control minimum/maximum size constraints are invalid", document, node.id,
                     "minimumSize"));
        }
        if (context.childPolicy && !node.parent.Empty() &&
            context.childPolicy(node.parent) != ui::ChildLayoutPolicy::Free &&
            node.properties.contains("offsets")) {
            Add(Make(diag::Severity::Warning, "PXEDUIP5015",
                     "This Container ignores the child's free-layout offsets", document,
                     node.id, "offsets",
                     ui::ChildLayoutPolicyName(context.childPolicy(node.parent))));
        }

        std::vector<const PropertyInfo*> descriptors;
        std::string type = node.type;
        while (const auto* info = TypeRegistry::Global().Find(type)) {
            for (const auto& property : info->properties) descriptors.push_back(&property);
            type = info->base;
            if (type.empty()) break;
        }
        for (const auto* property : descriptors) {
            const auto found = node.properties.find(property->name);
            if (found == node.properties.end()) continue;
            const std::string expected = ExpectedResourceType(*property);
            std::string path;
            if (const auto* text = found->second.TryGet<std::string>()) path = *text;
            if (const auto* reference = found->second.TryGet<ResourceRefValue>()) {
                path = reference->lastKnownPath;
            }
            if (!expected.empty() && !path.empty() && context.resourceExists &&
                !context.resourceExists(path, expected)) {
                Add(Make(diag::Severity::Error, "PXEDUIP5016",
                         "Resource is missing or has the wrong type", document, node.id,
                         property->name, path));
            }
        }

        if (const auto found = node.properties.find("triggers"); found != node.properties.end()) {
            const auto* triggers = found->second.AsObject();
            if (!triggers) {
                Add(Make(diag::Severity::Error, "PXEDUIP5020", "triggers must be a typed object",
                         document, node.id, "triggers"));
            } else {
                for (const auto& [signal, value] : *triggers) {
                    if(!TypeRegistry::Global().FindSignal(node.type,signal)){
                        Add(Make(diag::Severity::Error,"PXEDUIP5024",
                                 "Control does not expose this signal",document,node.id,
                                  "triggers."+signal));continue;
                    }
                    auto parsed=ui::ParseTriggerBinding(node.id,signal,value,
                                                       document.Path().generic_string());
                    if(!parsed){for(auto diagnostic:parsed.Diagnostics()){diagnostic.source.resourceId=document.DocumentId().ToString();diagnostic.source.path=document.Path().generic_string();Add(std::move(diagnostic));}continue;}
                    const auto& trigger=parsed.Value();
                    if(trigger.kind==ui::TriggerBindingKind::Flow){
                        const auto graphIt=document.Data().properties.find("interactionGraph");
                        if(graphIt==document.Data().properties.end()){
                            Add(Make(diag::Severity::Error,"PXEDUIP5025",
                                     "Behavior handler requires an embedded graph",document,
                                      node.id,"triggers."+signal));continue;}
                        const auto graph=ui::ParseBehaviorGraph(graphIt->second,
                                                                document.Path().generic_string());
                        if(!graph||!graph.Value().Find(trigger.graphEntry))Add(Make(
                            diag::Severity::Error,"PXEDUIP5026",
                            "Behavior entry is missing or graph is invalid",document,node.id,
                             "triggers."+signal));
                    } else if (context.validateAction) {
                        diag::Source source{.resourceId = document.DocumentId().ToString(),
                                            .path = document.Path().generic_string(),
                                            .nodeId = node.id.ToString(),
                                             .property = "triggers." + signal};
                        for (auto diagnostic :
                             context.validateAction(trigger.action, trigger.arguments, source)) {
                            Add(std::move(diagnostic));
                        }
                    }
                }
            }
        }

        if (const auto found = node.properties.find("bindings");
            found != node.properties.end()) {
            const auto* bindings = found->second.AsObject();
            if (!bindings) {
                Add(Make(diag::Severity::Error, "PXEDUIP5030",
                         "bindings must be a typed object", document, node.id, "bindings"));
            } else {
                for (const auto& [targetName, value] : *bindings) {
                    const auto* definition = value.AsObject();
                    const std::string* path = nullptr;
                    if (definition) {
                        const auto pathIt = definition->find("path");
                        if (pathIt != definition->end()) {
                            path = pathIt->second.TryGet<std::string>();
                        }
                    }
                    const auto* target = TypeRegistry::Global().FindProperty(node.type, targetName);
                    if (!target) {
                        Add(Make(diag::Severity::Error, "PXEDUIP5031",
                                 "Binding target property no longer exists", document, node.id,
                                 "bindings." + targetName));
                        continue;
                    }
                    if (!path || path->empty()) {
                        Add(Make(diag::Severity::Error, "PXEDUIP5032",
                                 "Binding path is missing", document, node.id,
                                 "bindings." + targetName));
                        continue;
                    }
                    const auto source = context.describeBinding
                                            ? context.describeBinding(*path)
                                            : std::optional<ui::PropertyPathInfo>{};
                    if (context.describeBinding && !source) {
                        Add(Make(diag::Severity::Error, "PXEDUIP5033",
                                 "Binding path cannot be resolved", document, node.id,
                                 "bindings." + targetName, *path));
                        continue;
                    }
                    VariantType outputType = source ? source->type : target->type;
                    if (definition) {
                        if (const auto formatterIt = definition->find("formatter");
                            formatterIt != definition->end()) {
                            if (const auto* name = formatterIt->second.TryGet<std::string>();
                                name && !name->empty() && context.findFormatter) {
                                const auto* formatter = context.findFormatter(*name);
                                if (!formatter) {
                                    Add(Make(diag::Severity::Error, "PXEDUIP5034",
                                             "Binding formatter is missing", document, node.id,
                                             "bindings." + targetName, *name));
                                } else if (source && formatter->input != source->type) {
                                    Add(Make(diag::Severity::Error, "PXEDUIP5035",
                                             "Binding formatter input type is incompatible",
                                             document, node.id, "bindings." + targetName, *name));
                                } else {
                                    outputType = formatter->output;
                                }
                            }
                        }
                    }
                    if (source && !Compatible(target->type, outputType)) {
                        Add(Make(diag::Severity::Error, "PXEDUIP5036",
                                 "Binding result type is incompatible with its target", document,
                                 node.id, "bindings." + targetName, *path));
                    }
                }
            }
        }
    }

    ValidateTokens(document);
    if (context.components) {
        for (auto diagnostic : context.components->Validate(document)) {
            Add(std::move(diagnostic));
        }
    }
    if (const auto found=document.Data().properties.find("animations");found!=document.Data().properties.end()) {
        auto animations = ui::ParseUIAnimationLibrary(found->second,document.Path().generic_string());
        if (!animations) {
            for (auto diagnostic : animations.Diagnostics()) {
                diagnostic.source.path = document.Path().generic_string();
                diagnostic.source.resourceId = document.DocumentId().ToString();
                Add(std::move(diagnostic));
            }
        }
    }
}

std::vector<const diag::Diagnostic*> DesignerDiagnostics::ForNode(const Uuid& node) const {
    std::vector<const diag::Diagnostic*> result;
    const auto [begin, end] = m_byNode.equal_range(node.ToString());
    for (auto it = begin; it != end; ++it) result.push_back(&m_items[it->second]);
    return result;
}

std::vector<const diag::Diagnostic*> DesignerDiagnostics::ForProperty(
    const Uuid& node, std::string_view property) const {
    auto result = ForNode(node);
    std::erase_if(result, [&](const auto* item) { return item->source.property != property; });
    return result;
}

bool DesignerDiagnostics::HasProblem(const Uuid& node) const {
    return m_byNode.contains(node.ToString());
}

std::size_t DesignerDiagnostics::ErrorCount() const {
    return static_cast<std::size_t>(std::count_if(m_items.begin(), m_items.end(),
                                                  [](const auto& item) {
                                                      return item.severity >= diag::Severity::Error;
                                                  }));
}

std::size_t DesignerDiagnostics::WarningCount() const {
    return static_cast<std::size_t>(std::count_if(m_items.begin(), m_items.end(),
                                                  [](const auto& item) {
                                                      return item.severity == diag::Severity::Warning;
                                                  }));
}

}  // namespace px::editor
