#include "Engine/UI/UIResourceResolver.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/Styles/StyleSerialization.h"

#include <algorithm>
#include <unordered_set>

namespace px::ui {
namespace {

diag::Diagnostic Error(std::string code, std::string message, std::string detail = {}) {
    return diag::Diagnostic{.severity = diag::Severity::Error, .code = std::move(code),
        .category = "UI.Resources", .message = std::move(message), .details = std::move(detail)};
}

Result<Variant> ResolveValue(const Variant& value, const VariantObject& tokens,
                             std::unordered_set<std::string>& resolving) {
    if (const auto* reference = value.TryGet<TokenRefValue>()) {
        const auto found = tokens.find(reference->name);
        if (found == tokens.end())
            return Result<Variant>::Failure(Error("PXUI3001", "Theme token does not exist", reference->name));
        if (!resolving.insert(reference->name).second)
            return Result<Variant>::Failure(Error("PXUI3002", "Theme token cycle detected", reference->name));
        auto resolved = ResolveValue(found->second, tokens, resolving);
        resolving.erase(reference->name);
        return resolved;
    }
    if (const auto* array = value.AsArray()) {
        VariantArray result; result.reserve(array->size());
        for (const auto& item : *array) {
            auto resolved = ResolveValue(item, tokens, resolving); if (!resolved) return resolved;
            result.push_back(resolved.TakeValue());
        }
        return Result<Variant>::Success(Variant(std::move(result)));
    }
    if (const auto* object = value.AsObject()) {
        VariantObject result;
        for (const auto& [name, item] : *object) {
            auto resolved = ResolveValue(item, tokens, resolving); if (!resolved) return resolved;
            result.emplace(name, resolved.TakeValue());
        }
        return Result<Variant>::Success(Variant(std::move(result)));
    }
    return Result<Variant>::Success(value.Clone());
}

struct ExposedProperty {
    std::string id;
    Uuid node;
    std::string property;
};
struct ExposedSignal {
    std::string id;
    Uuid node;
    std::string signal;
};
struct ComponentSlot {
    std::string id;
    Uuid node;
};

template <typename Definition, typename Reader>
Result<std::vector<Definition>> ReadComponentDefinitions(const resource::TypedDocument& component,
                                                          const char* field, Reader read) {
    std::vector<Definition> result;
    const auto found=component.properties.find(field);if(found==component.properties.end())return Result<std::vector<Definition>>::Success({});
    const auto* values=found->second.AsArray();if(!values)return Result<std::vector<Definition>>::Failure(Error("PXUI3017",std::string(field)+" must be an Array"));
    std::unordered_set<std::string> ids;
    for(const auto& value:*values){const auto* object=value.AsObject();if(!object)return Result<std::vector<Definition>>::Failure(Error("PXUI3018",std::string(field)+" entries must be Objects"));Definition definition;if(!read(*object,definition)||definition.id.empty()||definition.node.Empty()||!ids.insert(definition.id).second)return Result<std::vector<Definition>>::Failure(Error("PXUI3019",std::string(field)+" contains an invalid or duplicate definition"));result.push_back(std::move(definition));}
    return Result<std::vector<Definition>>::Success(std::move(result));
}

const resource::NodeRecord* FindNode(const resource::TypedDocument& document,const Uuid& id){
    const auto found=std::find_if(document.nodes.begin(),document.nodes.end(),[&](const auto& node){return node.id==id;});return found==document.nodes.end()?nullptr:&*found;
}

Result<std::vector<ExposedProperty>> ReadExposedProperties(const resource::TypedDocument& component){
    return ReadComponentDefinitions<ExposedProperty>(component,"component.exposedProperties",[&](const VariantObject& object,ExposedProperty& output){const auto id=object.find("id"),node=object.find("node"),property=object.find("property");if(id==object.end()||node==object.end()||property==object.end()||!id->second.TryGet<std::string>()||!node->second.TryGet<Uuid>()||!property->second.TryGet<std::string>())return false;output={*id->second.TryGet<std::string>(),*node->second.TryGet<Uuid>(),*property->second.TryGet<std::string>()};const auto* record=FindNode(component,output.node);return record&&TypeRegistry::Global().FindProperty(record->type,output.property);});
}
Result<std::vector<ExposedSignal>> ReadExposedSignals(const resource::TypedDocument& component){
    return ReadComponentDefinitions<ExposedSignal>(component,"component.exposedSignals",[&](const VariantObject& object,ExposedSignal& output){const auto id=object.find("id"),node=object.find("node"),signal=object.find("signal");if(id==object.end()||node==object.end()||signal==object.end()||!id->second.TryGet<std::string>()||!node->second.TryGet<Uuid>()||!signal->second.TryGet<std::string>())return false;output={*id->second.TryGet<std::string>(),*node->second.TryGet<Uuid>(),*signal->second.TryGet<std::string>()};const auto* record=FindNode(component,output.node);return record&&TypeRegistry::Global().FindSignal(record->type,output.signal);});
}
Result<std::vector<ComponentSlot>> ReadComponentSlots(const resource::TypedDocument& component){
    return ReadComponentDefinitions<ComponentSlot>(component,"component.slots",[&](const VariantObject& object,ComponentSlot& output){const auto id=object.find("id"),node=object.find("node");if(id==object.end()||node==object.end()||!id->second.TryGet<std::string>()||!node->second.TryGet<Uuid>())return false;output={*id->second.TryGet<std::string>(),*node->second.TryGet<Uuid>()};return FindNode(component,output.node)!=nullptr;});
}

Result<ExpandedUIDocument> ExpandDocument(const resource::TypedDocument& source,
                                           const UIDocumentLoader& loader,
                                           std::unordered_set<Uuid, UuidHash>& stack) {
    ExpandedUIDocument output; output.document = source; output.document.nodes.clear();
    std::unordered_map<Uuid,std::unordered_map<std::string,Uuid>,UuidHash> slotTargets;
    for (const auto& sourceRecord : source.nodes) {
        resource::NodeRecord record=sourceRecord;
        if(const auto slot=record.properties.find("componentSlot");slot!=record.properties.end()){
            const auto* name=slot->second.TryGet<std::string>();const auto parent=slotTargets.find(record.parent);
            if(!name||parent==slotTargets.end()||!parent->second.contains(*name))return Result<ExpandedUIDocument>::Failure(Error("PXUI3023","Control references a missing Component slot",record.name));
            record.parent=parent->second.at(*name);record.properties.erase("componentSlot");
        }
        if (record.type != "ComponentInstance") {
            output.document.nodes.push_back(std::move(record));
            continue;
        }
        const auto componentIt = record.properties.find("component");
        const auto* reference = componentIt == record.properties.end()
                                    ? nullptr : componentIt->second.TryGet<ResourceRefValue>();
        if (!reference || reference->id.Empty())
            return Result<ExpandedUIDocument>::Failure(
                Error("PXUI3010", "ComponentInstance requires a component ResourceRef", record.name));
        if (!loader)
            return Result<ExpandedUIDocument>::Failure(
                Error("PXUI3011", "No UI resource loader was provided", reference->lastKnownPath));
        if (!stack.insert(reference->id).second)
            return Result<ExpandedUIDocument>::Failure(
                Error("PXUI3012", "Component dependency cycle detected", reference->lastKnownPath));
        auto loaded = loader(*reference);
        if (!loaded) { stack.erase(reference->id); return Result<ExpandedUIDocument>::Failure(loaded.Diagnostics()); }
        if (loaded.Value().kind != resource::DocumentKind::Scene || loaded.Value().type != "UIComponent") {
            stack.erase(reference->id);
            return Result<ExpandedUIDocument>::Failure(
                Error("PXUI3013", "Referenced resource is not a UIComponent", reference->lastKnownPath));
        }
        const auto schema=loaded.Value().properties.find("uiSchemaVersion");
        const auto* version=schema==loaded.Value().properties.end()?nullptr:
            schema->second.TryGet<std::int64_t>();
        if(!version||*version!=5){stack.erase(reference->id);return Result<ExpandedUIDocument>::Failure(
            Error("PXUI3016","UIComponent requires strict uiSchemaVersion 5",
                  reference->lastKnownPath));}
        auto expanded = ExpandDocument(loaded.Value(), loader, stack);
        stack.erase(reference->id);
        if (!expanded) return expanded;

        const resource::NodeRecord* sourceRoot = nullptr;
        for (const auto& node : expanded.Value().document.nodes) {
            if (!node.parent.Empty()) continue;
            if (sourceRoot)
                return Result<ExpandedUIDocument>::Failure(
                    Error("PXUI3014", "UIComponent must have exactly one root",
                          reference->lastKnownPath));
            sourceRoot = &node;
        }
        if (!sourceRoot) return Result<ExpandedUIDocument>::Failure(
            Error("PXUI3014", "UIComponent root is missing", reference->lastKnownPath));

        auto exposedProperties=ReadExposedProperties(loaded.Value());if(!exposedProperties)return Result<ExpandedUIDocument>::Failure(exposedProperties.Diagnostics());
        auto exposedSignals=ReadExposedSignals(loaded.Value());if(!exposedSignals)return Result<ExpandedUIDocument>::Failure(exposedSignals.Diagnostics());
        auto slots=ReadComponentSlots(loaded.Value());if(!slots)return Result<ExpandedUIDocument>::Failure(slots.Diagnostics());
        const VariantObject* propertyValues=nullptr;if(const auto it=record.properties.find("componentProperties");it!=record.properties.end()){propertyValues=it->second.AsObject();if(!propertyValues)return Result<ExpandedUIDocument>::Failure(Error("PXUI3024","componentProperties must be an Object",record.name));}
        const VariantObject* signalValues=nullptr;if(const auto it=record.properties.find("componentEvents");it!=record.properties.end()){signalValues=it->second.AsObject();if(!signalValues)return Result<ExpandedUIDocument>::Failure(Error("PXUI3025","componentEvents must be an Object",record.name));}
        if(propertyValues)for(const auto& [id,_]:*propertyValues)if(std::none_of(exposedProperties.Value().begin(),exposedProperties.Value().end(),[&](const auto& definition){return definition.id==id;}))return Result<ExpandedUIDocument>::Failure(Error("PXUI3026","Component property is not exposed",id));
        if(signalValues)for(const auto& [id,_]:*signalValues)if(std::none_of(exposedSignals.Value().begin(),exposedSignals.Value().end(),[&](const auto& definition){return definition.id==id;}))return Result<ExpandedUIDocument>::Failure(Error("PXUI3027","Component signal is not exposed",id));

        std::unordered_map<Uuid, Uuid, UuidHash> ids;
        for (const auto& node : expanded.Value().document.nodes)
            ids[node.id] = node.id == sourceRoot->id ? record.id
                : Uuid::FromName(record.id.ToString() + "/" + node.id.ToString());
        const VariantObject* overrides = nullptr;
        if (const auto it = record.properties.find("overrides"); it != record.properties.end())
            overrides = it->second.AsObject();
        for(const auto& slot:slots.Value()){const auto target=ids.find(slot.node);if(target==ids.end())return Result<ExpandedUIDocument>::Failure(Error("PXUI3028","Component slot target disappeared during expansion",slot.id));slotTargets[record.id][slot.id]=target->second;}
        for (auto node : expanded.Value().document.nodes) {
            const Uuid sourceId = node.id;
            node.id = ids.at(sourceId);
            node.parent = sourceId == sourceRoot->id ? record.parent : ids.at(node.parent);
            if (sourceId == sourceRoot->id) {
                for (const auto& [name, value] : record.properties)
                    if (name != "component" && name != "overrides" && name != "componentProperties" && name != "componentEvents") node.properties[name] = value.Clone();
                node.name = record.name;
            }
            if(propertyValues)for(const auto& definition:exposedProperties.Value())if(definition.node==sourceId)if(const auto value=propertyValues->find(definition.id);value!=propertyValues->end())node.properties[definition.property]=value->second.Clone();
            if(signalValues)for(const auto& definition:exposedSignals.Value())if(definition.node==sourceId)if(const auto value=signalValues->find(definition.id);value!=signalValues->end()){VariantObject triggers;if(const auto existing=node.properties.find("triggers");existing!=node.properties.end()&&existing->second.AsObject())triggers=*existing->second.AsObject();triggers[definition.signal]=value->second.Clone();node.properties["triggers"]=std::move(triggers);}
            if (overrides) {
                if (const auto found = overrides->find(sourceId.ToString()); found != overrides->end()) {
                    const auto* values = found->second.AsObject();
                    if (!values) return Result<ExpandedUIDocument>::Failure(
                        Error("PXUI3015", "Component override must be an object", sourceId.ToString()));
                    for (const auto& [name, value] : *values) node.properties[name] = value.Clone();
                }
            }
            output.origins.push_back({node.id, record.id, sourceId});
            output.document.nodes.push_back(std::move(node));
        }
    }
    return Result<ExpandedUIDocument>::Success(std::move(output));
}
}

Result<ExpandedUIDocument> ExpandUIComponents(const resource::TypedDocument& source,
                                               const UIDocumentLoader& loader) {
    std::unordered_set<Uuid, UuidHash> stack;
    return ExpandDocument(source, loader, stack);
}

Status ValidateUIComponentInterface(const resource::TypedDocument& component){
    if(component.kind!=resource::DocumentKind::Scene||component.type!="UIComponent")return Status::Fail(Error("PXUI3029","Component asset must be a typed UIComponent scene"));
    const auto schema=component.properties.find("uiSchemaVersion");const auto* version=schema==component.properties.end()?nullptr:schema->second.TryGet<std::int64_t>();if(!version||*version!=5)return Status::Fail(Error("PXUI3030","UIComponent requires strict uiSchemaVersion 5"));
    std::size_t roots=0;for(const auto& node:component.nodes)if(node.parent.Empty())++roots;if(roots!=1)return Status::Fail(Error("PXUI3031","UIComponent must contain exactly one root Control"));
    auto properties=ReadExposedProperties(component);if(!properties)return Status::Fail(properties.Diagnostics());auto signals=ReadExposedSignals(component);if(!signals)return Status::Fail(signals.Diagnostics());auto slots=ReadComponentSlots(component);if(!slots)return Status::Fail(slots.Diagnostics());return Status::Ok();
}

Result<Variant> ResolveThemeValue(const Variant& value, const VariantObject& tokens) {
    std::unordered_set<std::string> resolving;
    return ResolveValue(value, tokens, resolving);
}

Result<Theme> LoadUITheme(const resource::TypedDocument& document) {
    if (document.kind != resource::DocumentKind::Resource || document.type != "UITheme")
        return Result<Theme>::Failure(Error("PXUI3020", "Theme resource must be @pxresource UITheme"));
    if (document.properties.contains("tokens") || document.properties.contains("styles"))
        return Result<Theme>::Failure(Error("PXUI3021",
            "Legacy UITheme tokens/styles are forbidden; use styleSystem"));
    const auto styleSystem=document.properties.find("styleSystem");
    if(styleSystem==document.properties.end())
        return Result<Theme>::Failure(Error("PXUI3022","UITheme requires styleSystem version 3"));
    auto parsed=ParseStyleTheme(styleSystem->second);
    if(!parsed)return Result<Theme>::Failure(parsed.Diagnostics());
    Theme theme;const Status installed=theme.SetStyleData(parsed.TakeValue());
    if(!installed)return Result<Theme>::Failure(installed.Diagnostics());
    return Result<Theme>::Success(std::move(theme));
}

}  // namespace px::ui