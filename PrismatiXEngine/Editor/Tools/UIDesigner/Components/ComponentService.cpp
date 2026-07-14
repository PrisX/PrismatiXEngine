#include "Editor/Tools/UIDesigner/Components/ComponentService.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/Control.h"

#include <algorithm>
#include <unordered_set>

namespace px::editor {
namespace {

bool AppendCaptured(const VariantObject& value, const Uuid& parent,
                    std::vector<resource::NodeRecord>& output) {
    const auto idIt = value.find("id");
    const auto nameIt = value.find("name");
    const auto typeIt = value.find("type");
    const auto propertiesIt = value.find("properties");
    if (idIt == value.end() || nameIt == value.end() || typeIt == value.end() ||
        propertiesIt == value.end()) {
        return false;
    }
    const auto* id = idIt->second.TryGet<Uuid>();
    const auto* name = nameIt->second.TryGet<std::string>();
    const auto* type = typeIt->second.TryGet<std::string>();
    const auto* properties = propertiesIt->second.AsObject();
    if (!id || !name || !type || !properties) return false;
    output.push_back({*id, parent, *name, *type, *properties});
    if (const auto childrenIt = value.find("children"); childrenIt != value.end()) {
        if (const auto* children = childrenIt->second.AsArray()) {
            for (const auto& childValue : *children) {
                const auto* child = childValue.AsObject();
                if (!child || !AppendCaptured(*child, *id, output)) return false;
            }
        }
    }
    return true;
}

VariantObject CaptureExpanded(const std::vector<resource::NodeRecord>& nodes, const Uuid& id) {
    const auto found = std::find_if(nodes.begin(), nodes.end(),
                                    [&](const auto& node) { return node.id == id; });
    if (found == nodes.end()) return {};
    VariantArray children;
    for (const auto& child : nodes) {
        if (child.parent == id) children.emplace_back(CaptureExpanded(nodes, child.id));
    }
    return {{"id", found->id},
            {"name", found->name},
            {"type", found->type},
            {"properties", VariantObject(found->properties)},
            {"children", std::move(children)}};
}

Variant ExistingProperty(const resource::NodeRecord& node, const std::string& property) {
    const auto found = node.properties.find(property);
    return found == node.properties.end() ? Variant{} : found->second.Clone();
}

bool CompatibleValue(VariantType expected, VariantType actual) {
    if (expected == actual) return true;
    return expected == VariantType::Number && actual == VariantType::Integer;
}

}  // namespace

diag::Diagnostic ComponentService::Problem(diag::Severity severity, std::string code,
                                           std::string message,
                                           const UISceneDocument& document, const Uuid& node,
                                           std::string property) {
    diag::Diagnostic result{.severity = severity,
                            .code = std::move(code),
                            .category = "Editor.UIComponent",
                            .message = std::move(message)};
    result.source.path = document.Path().generic_string();
    result.source.resourceId = document.DocumentId().ToString();
    if (!node.Empty()) result.source.nodeId = node.ToString();
    result.source.property = std::move(property);
    return result;
}

Result<resource::TypedDocument> ComponentService::Load(const ResourceRefValue& reference) const {
    if (!m_loader) {
        return Result<resource::TypedDocument>::Failure(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXEDCOMP4001",
            .category = "Editor.UIComponent",
            .message = "No component resource loader is configured",
            .details = reference.lastKnownPath});
    }
    return m_loader(reference);
}

Status ComponentService::CreateFromSelection(UISceneDocument& document, const Uuid& selected,
                                             const Rect& visualRect,
                                             const std::filesystem::path& componentPath) const {
    const auto* selectedNode = document.Find(selected);
    if (!selectedNode || selectedNode->parent.Empty() || !m_writer) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4002",
                                    "Select a non-root Control before creating a component",
                                    document, selected));
    }
    auto captured = document.CaptureSubtree(selected);
    if (!captured) return Status::Fail(captured.Diagnostics());

    resource::TypedDocument component;
    component.kind = resource::DocumentKind::Scene;
    component.id = Uuid::Random();
    component.type = "UIComponent";
    component.properties["canvasSize"] =
        Vec2{std::max(1.0f, visualRect.w), std::max(1.0f, visualRect.h)};
    component.properties["uiSchemaVersion"] = std::int64_t{5};
    if (!AppendCaptured(captured.Value(), {}, component.nodes) || component.nodes.empty()) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4003",
                                    "The selected hierarchy cannot be serialized as a component",
                                    document, selected));
    }

    // A component root is local to its own artboard.  Instance placement stays on
    // the instance envelope, so edits to the source root are never invisibly frozen.
    auto& sourceRoot = component.nodes.front();
    sourceRoot.properties["anchors"] = Rect{0, 0, 1, 1};
    sourceRoot.properties["offsets"] = Rect{0, 0, 0, 0};

    auto written = m_writer(componentPath, resource::WriteTypedDocument(component));
    if (!written) return Status::Fail(written.Diagnostics());

    VariantObject instanceProperties{{"component", written.Value()},
                                     {"overrides", VariantObject{}},
                                     {"componentProperties",VariantObject{}},
                                     {"componentEvents",VariantObject{}}};
    for (const char* placement : {"anchors", "offsets", "minimumSize", "maximumSize",
                                  "stretchRatio"}) {
        if (const auto found = selectedNode->properties.find(placement);
            found != selectedNode->properties.end()) {
            instanceProperties[placement] = found->second.Clone();
        }
    }
    VariantObject instance{{"id", selected},
                           {"name", selectedNode->name},
                           {"type", std::string("ComponentInstance")},
                           {"properties", std::move(instanceProperties)},
                           {"children", VariantArray{}}};

    const Uuid parent = selectedNode->parent;
    const std::size_t index = document.ChildIndex(selected);
    auto command = std::make_unique<CompositeEditCommand>("Create Component from Selection");
    command->Add(std::make_unique<SubtreeEditCommand>(
        "Remove component source hierarchy", SubtreeOperation::Remove, selected, parent, index,
        captured.TakeValue()));
    command->Add(std::make_unique<SubtreeEditCommand>(
        "Insert component instance", SubtreeOperation::Insert, selected, parent, index,
        std::move(instance)));
    return document.History().Execute(std::move(command));
}

Status ComponentService::Instantiate(UISceneDocument& document,
                                     const ResourceRefValue& component, const Uuid& parent,
                                     std::size_t index, Rect offsets) const {
    auto loaded = Load(component);
    if (!loaded) return Status::Fail(loaded.Diagnostics());
    if (loaded.Value().kind != resource::DocumentKind::Scene ||
        loaded.Value().type != "UIComponent") {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4004",
                                    "The selected resource is not a UIComponent", document,
                                    parent, "component"));
    }
    resource::TypedDocument probe = document.Data();
    const Uuid id = Uuid::Random();
    const std::string name = loaded.Value().nodes.empty()
                                 ? "ComponentInstance"
                                 : loaded.Value().nodes.front().name;
    VariantObject subtree{{"id", id},
                          {"name", name},
                          {"type", std::string("ComponentInstance")},
                          {"properties",
                           VariantObject{{"component", component},
                                         {"overrides", VariantObject{}},
                                         {"componentProperties",VariantObject{}},
                                         {"componentEvents",VariantObject{}},
                                         {"anchors", Rect{}},
                                         {"offsets", offsets}}},
                          {"children", VariantArray{}}};
    auto command = std::make_unique<SubtreeEditCommand>(
        "Instantiate Component", SubtreeOperation::Insert, id, parent, index,
        std::move(subtree));
    return document.History().Execute(std::move(command));
}

Status ComponentService::SetPropertyOverride(UISceneDocument& document, const Uuid& instance,
                                             const Uuid& sourceNode,
                                             const std::string& property,
                                             const Variant& value) const {
    const auto* node = document.Find(instance);
    if (!node || node->type != "ComponentInstance") {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4005",
                                    "Property overrides require a ComponentInstance", document,
                                    instance, property));
    }
    const auto componentIt = node->properties.find("component");
    const auto* reference = componentIt == node->properties.end()
                                ? nullptr
                                : componentIt->second.TryGet<ResourceRefValue>();
    if (!reference) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4006",
                                    "ComponentInstance has no valid component reference", document,
                                    instance, "component"));
    }
    auto source = Load(*reference);
    if (!source) return Status::Fail(source.Diagnostics());
    const auto sourceIt = std::find_if(source.Value().nodes.begin(), source.Value().nodes.end(),
                                       [&](const auto& item) { return item.id == sourceNode; });
    if (sourceIt == source.Value().nodes.end()) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4007",
                                    "Component override target no longer exists", document,
                                    instance, property));
    }
    const auto* descriptor = TypeRegistry::Global().FindProperty(sourceIt->type, property);
    if (!descriptor || !HasFlag(descriptor->flags, PropertyFlags::Editable)) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4008",
                                    "Component property is not editable", document, instance,
                                    property));
    }
    if (!CompatibleValue(descriptor->type, value.Type()) && value.Type() != VariantType::Null) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4009",
                                    "Component override has the wrong value type", document,
                                    instance, property));
    }

    Variant before = ExistingProperty(*node, "overrides");
    VariantObject changed;
    if (const auto* existing = before.AsObject()) changed = *existing;
    VariantObject values;
    const std::string sourceKey = sourceNode.ToString();
    if (const auto found = changed.find(sourceKey); found != changed.end()) {
        if (const auto* existing = found->second.AsObject()) values = *existing;
    }
    values[property] = value.Clone();
    changed[sourceKey] = std::move(values);
    auto command = std::make_unique<PropertyChangeCommand>(
        "Set Component Property Override", instance, "overrides", std::move(before),
        Variant(std::move(changed)), std::chrono::steady_clock::now(), false);
    return document.History().Execute(std::move(command));
}

Status ComponentService::ResetPropertyOverride(UISceneDocument& document, const Uuid& instance,
                                               const Uuid& sourceNode,
                                               const std::string& property) const {
    const auto* node = document.Find(instance);
    if (!node || node->type != "ComponentInstance") return Status::Ok();
    Variant before = ExistingProperty(*node, "overrides");
    const auto* existing = before.AsObject();
    if (!existing) return Status::Ok();
    VariantObject changed = *existing;
    const std::string key = sourceNode.ToString();
    const auto found = changed.find(key);
    if (found == changed.end()) return Status::Ok();
    auto* fields = found->second.AsObject();
    if (!fields || !fields->contains(property)) return Status::Ok();
    fields->erase(property);
    if (fields->empty()) changed.erase(key);
    auto command = std::make_unique<PropertyChangeCommand>(
        "Reset Component Property Override", instance, "overrides", std::move(before),
        Variant(std::move(changed)), std::chrono::steady_clock::now(), false);
    return document.History().Execute(std::move(command));
}

Status ComponentService::ResetAllOverrides(UISceneDocument& document,
                                           const Uuid& instance) const {
    const auto* node = document.Find(instance);
    if (!node || node->type != "ComponentInstance") return Status::Ok();
    Variant before = ExistingProperty(*node, "overrides");
    if (!before.AsObject() || before.AsObject()->empty()) return Status::Ok();
    auto command = std::make_unique<PropertyChangeCommand>(
        "Reset All Component Overrides", instance, "overrides", std::move(before),
        Variant(VariantObject{}), std::chrono::steady_clock::now(), false);
    return document.History().Execute(std::move(command));
}

Status ComponentService::Detach(UISceneDocument& document, const Uuid& instance) const {
    const auto* node = document.Find(instance);
    if (!node || node->type != "ComponentInstance") return Status::Ok();
    resource::TypedDocument temporary = document.Data();
    temporary.nodes = {*node};
    temporary.nodes.front().parent = {};
    auto expanded = ui::ExpandUIComponents(temporary, m_loader);
    if (!expanded) return Status::Fail(expanded.Diagnostics());
    VariantObject materialized = CaptureExpanded(expanded.Value().document.nodes, instance);
    if (materialized.empty()) {
        return Status::Fail(Problem(diag::Severity::Error, "PXEDCOMP4010",
                                    "Expanded component hierarchy is empty", document, instance));
    }
    auto original = document.CaptureSubtree(instance);
    if (!original) return Status::Fail(original.Diagnostics());
    const Uuid parent = node->parent;
    const std::size_t index = document.ChildIndex(instance);
    auto command = std::make_unique<CompositeEditCommand>("Detach Component");
    command->Add(std::make_unique<SubtreeEditCommand>(
        "Remove Component Instance", SubtreeOperation::Remove, instance, parent, index,
        original.TakeValue()));
    command->Add(std::make_unique<SubtreeEditCommand>(
        "Materialize Component Hierarchy", SubtreeOperation::Insert, instance, parent, index,
        std::move(materialized)));
    return document.History().Execute(std::move(command));
}

std::vector<ComponentService::OverrideInfo> ComponentService::Overrides(
    const UISceneDocument& document, const Uuid& instance) const {
    std::vector<OverrideInfo> result;
    const auto* node = document.Find(instance);
    if (!node || node->type != "ComponentInstance") return result;
    const auto found = node->properties.find("overrides");
    const auto* overrides = found == node->properties.end() ? nullptr : found->second.AsObject();
    if (!overrides) return result;
    for (const auto& [sourceText, value] : *overrides) {
        const auto source = Uuid::Parse(sourceText);
        const auto* fields = value.AsObject();
        if (!source || !fields) continue;
        for (const auto& [property, fieldValue] : *fields) {
            result.push_back({instance, *source, property, fieldValue.Clone()});
        }
    }
    return result;
}

std::size_t ComponentService::OverrideCount(const UISceneDocument& document,
                                            const Uuid& instance) const {
    return Overrides(document, instance).size();
}

std::vector<diag::Diagnostic> ComponentService::Validate(
    const UISceneDocument& document) const {
    std::vector<diag::Diagnostic> diagnostics;
    if(document.Data().type=="UIComponent")if(const Status interfaceStatus=ui::ValidateUIComponentInterface(document.Data());!interfaceStatus)for(auto diagnostic:interfaceStatus.Diagnostics()){diagnostic.source.path=document.Path().generic_string();diagnostic.source.resourceId=document.DocumentId().ToString();diagnostics.push_back(std::move(diagnostic));}
    for (const auto& node : document.Data().nodes) {
        if (node.type != "ComponentInstance") continue;
        const auto componentIt = node.properties.find("component");
        const auto* reference = componentIt == node.properties.end()
                                    ? nullptr
                                    : componentIt->second.TryGet<ResourceRefValue>();
        if (!reference) {
            diagnostics.push_back(Problem(diag::Severity::Error, "PXEDCOMP4101",
                                          "Missing component reference", document, node.id,
                                          "component"));
            continue;
        }
        auto source = Load(*reference);
        if (!source) {
            auto problem = Problem(diag::Severity::Error, "PXEDCOMP4102",
                                   "Component resource is unavailable", document, node.id,
                                   "component");
            problem.details = reference->lastKnownPath;
            diagnostics.push_back(std::move(problem));
            continue;
        }
        std::unordered_set<Uuid, UuidHash> sourceIds;
        for (const auto& sourceNode : source.Value().nodes) sourceIds.insert(sourceNode.id);
        for (const auto& override : Overrides(document, node.id)) {
            const auto sourceNode = std::find_if(
                source.Value().nodes.begin(), source.Value().nodes.end(),
                [&](const auto& candidate) { return candidate.id == override.sourceNode; });
            if (sourceNode == source.Value().nodes.end()) {
                diagnostics.push_back(Problem(diag::Severity::Error, "PXEDCOMP4103",
                                              "Component override target is missing", document,
                                              node.id, override.property));
                continue;
            }
            const auto* property =
                TypeRegistry::Global().FindProperty(sourceNode->type, override.property);
            if (!property) {
                diagnostics.push_back(Problem(diag::Severity::Error, "PXEDCOMP4104",
                                              "Component override property is missing", document,
                                              node.id, override.property));
            } else if (!CompatibleValue(property->type, override.value.Type())) {
                diagnostics.push_back(Problem(diag::Severity::Error, "PXEDCOMP4105",
                                              "Component override type does not match its source",
                                              document, node.id, override.property));
            }
        }
        resource::TypedDocument probe;
        probe.kind = resource::DocumentKind::Scene;
        probe.id = document.DocumentId();
        probe.type = "UIScene";
        probe.nodes.push_back(node);
        probe.nodes.front().parent = {};
        if (auto expanded = ui::ExpandUIComponents(probe, m_loader); !expanded) {
            auto problem = Problem(diag::Severity::Error, "PXEDCOMP4106",
                                   "Component cannot be expanded (missing dependency or cycle)",
                                   document, node.id, "component");
            if (!expanded.Diagnostics().empty()) {
                problem.details = diag::Describe(expanded.Diagnostics().front());
            }
            diagnostics.push_back(std::move(problem));
        }
    }
    if(auto expanded=ui::ExpandUIComponents(document.Data(),m_loader);!expanded){
        auto problem=Problem(diag::Severity::Error,"PXEDCOMP4107",
                             "Component public interface or slot projection is invalid",
                             document);
        if(!expanded.Diagnostics().empty())problem.details=diag::Describe(expanded.Diagnostics().front());
        diagnostics.push_back(std::move(problem));
    }
    return diagnostics;
}

}  // namespace px::editor
