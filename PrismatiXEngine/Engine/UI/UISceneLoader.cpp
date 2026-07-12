#include "Engine/UI/UISceneLoader.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/ActionRegistry.h"
#include "Engine/UI/Widgets.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace px::ui {
namespace {
diag::Diagnostic SceneError(std::string code, std::string message, const resource::NodeRecord* record = nullptr,
                            std::string property = {}) {
    diag::Diagnostic d{.severity = diag::Severity::Error, .code = std::move(code),
                       .category = "UI.Scene", .message = std::move(message)};
    if (record) { d.source.nodeId = record->id.ToString(); d.source.property = std::move(property); }
    diag::Emit(d); return d;
}

Result<std::unique_ptr<Control>> CreateControl(const resource::NodeRecord& record) {
    auto object = TypeRegistry::Global().Create(record.type);
    if (!object) return Result<std::unique_ptr<Control>>::Failure(SceneError("PXUI2601", "Unknown UI node type: " + record.type, &record));
    auto* control = dynamic_cast<Control*>(object.get());
    if (!control) return Result<std::unique_ptr<Control>>::Failure(SceneError("PXUI2602", "UI scene node is not a Control: " + record.type, &record));
    object.release();
    std::unique_ptr<Control> value(control); value->SetId(record.id); value->SetName(record.name);
    return Result<std::unique_ptr<Control>>::Success(std::move(value));
}
}

Result<LoadedUIScene> InstantiateUIScene(const resource::TypedDocument& document, IViewModel* viewModel,
                                         const FormatterRegistry& formatters,
                                         const UIDocumentLoader& loader) {
    const Status registration = RegisterBuiltinUITypes();
    if (!registration) return Result<LoadedUIScene>::Failure(registration.Diagnostics());
    if (document.kind != resource::DocumentKind::Scene)
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2603", "UI loader requires an @pxscene document"));
    if (document.type != "UIScene" && document.type != "UIComponent")
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2622", "UI loader requires UIScene or UIComponent"));
    auto expanded = ExpandUIComponents(document, loader);
    if (!expanded) return Result<LoadedUIScene>::Failure(expanded.Diagnostics());
    const auto& scene = expanded.Value().document;
    if (scene.nodes.empty())
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2604", "UI scene has no nodes"));

    std::unordered_map<Uuid, const resource::NodeRecord*, UuidHash> records;
    const resource::NodeRecord* rootRecord = nullptr;
    for (const auto& record : scene.nodes) {
        if (!records.emplace(record.id, &record).second)
            return Result<LoadedUIScene>::Failure(SceneError("PXUI2605", "Duplicate node UUID in UI scene", &record));
        if (record.parent.Empty()) {
            if (rootRecord) return Result<LoadedUIScene>::Failure(SceneError("PXUI2606", "UI scene must have exactly one root Control", &record));
            rootRecord = &record;
        }
    }
    if (!rootRecord) return Result<LoadedUIScene>::Failure(SceneError("PXUI2607", "UI scene root is missing"));
    for (const auto& record : scene.nodes) if (!record.parent.Empty() && !records.contains(record.parent))
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2608", "UI node references a missing parent", &record));

    std::unordered_set<Uuid, UuidHash> building;
    std::function<Result<std::unique_ptr<Control>>(const resource::NodeRecord&)> build =
        [&](const resource::NodeRecord& record) -> Result<std::unique_ptr<Control>> {
            if (!building.insert(record.id).second)
                return Result<std::unique_ptr<Control>>::Failure(SceneError("PXUI2609", "Cycle detected in UI scene tree", &record));
            auto created = CreateControl(record); if (!created) return created;
            for (const auto& candidate : scene.nodes) if (candidate.parent == record.id) {
                auto child = build(candidate); if (!child) return child;
                const Status added = created.Value()->AddChild(std::move(child.Value()));
                if (!added) return Result<std::unique_ptr<Control>>::Failure(added.Diagnostics());
            }
            building.erase(record.id); return created;
        };
    auto root = build(*rootRecord); if (!root) return Result<LoadedUIScene>::Failure(root.Diagnostics());

    LoadedUIScene loaded; loaded.root = std::move(root.Value());
    if (const auto themeIt=scene.properties.find("theme"); themeIt!=scene.properties.end()) {
        const auto* reference=themeIt->second.TryGet<ResourceRefValue>();
        if(!reference||!loader)return Result<LoadedUIScene>::Failure(SceneError("PXUI2623","Scene theme requires a ResourceRef and resource loader"));
        auto themeDocument=loader(*reference);if(!themeDocument)return Result<LoadedUIScene>::Failure(themeDocument.Diagnostics());
        auto theme=LoadUITheme(themeDocument.Value());if(!theme)return Result<LoadedUIScene>::Failure(theme.Diagnostics());loaded.theme=theme.TakeValue();
    }
    if(scene.properties.contains("animation.tracks")){auto animation=LoadEmbeddedAnimation(scene);if(!animation)return Result<LoadedUIScene>::Failure(animation.Diagnostics());loaded.animation=std::move(animation.Value());}
    for (const auto& record : scene.nodes) {
        auto* object = loaded.root->Find(record.id);
        if (!object) return Result<LoadedUIScene>::Failure(SceneError("PXUI2610", "Instantiated UI node could not be resolved", &record));
        for (const auto& [propertyName, value] : record.properties) {
            if (propertyName=="bindings" || propertyName=="events" || propertyName=="editorLocked") continue;
            const auto* property = TypeRegistry::Global().FindProperty(record.type, propertyName);
            if (!property || !property->set)
                return Result<LoadedUIScene>::Failure(SceneError("PXUI2611", "Unknown UI property " + record.type + "." + propertyName, &record, propertyName));
            if(propertyName=="command")return Result<LoadedUIScene>::Failure(SceneError("PXUI2621","Direct command properties are forbidden; use a typed EventBinding",&record,propertyName));
            Variant resolved=value.Clone();
            if (value.Type()==VariantType::TokenRef) {
                if(!loaded.theme)return Result<LoadedUIScene>::Failure(SceneError("PXUI2624","Token property requires a scene theme",&record,propertyName));
                const auto* token=loaded.theme->FindToken(value.TryGet<TokenRefValue>()->name);
                if(!token)return Result<LoadedUIScene>::Failure(SceneError("PXUI2625","Theme token does not exist",&record,propertyName));
                resolved=token->Clone();
                if(resolved.Type()!=property->type)return Result<LoadedUIScene>::Failure(SceneError("PXUI2626","Theme token type does not match property",&record,propertyName));
            }
            const Status set = property->set(*object, resolved);
            if (!set) return Result<LoadedUIScene>::Failure(set.Diagnostics());
        }
        if(const auto found=record.properties.find("bindings");found!=record.properties.end()){const auto* definitions=found->second.AsObject();if(!definitions)return Result<LoadedUIScene>::Failure(SceneError("PXUI2613","bindings must be an Object",&record,"bindings"));if(!viewModel)return Result<LoadedUIScene>::Failure(SceneError("PXUI2612","Scene contains bindings but no ViewModel was provided",&record,"bindings"));for(const auto& [targetName,value]:*definitions){const auto* definition=value.AsObject();if(!definition)return Result<LoadedUIScene>::Failure(SceneError("PXUI2613","Binding descriptor must be an Object",&record,targetName));const auto pathValue=definition->find("path");const auto* path=pathValue!=definition->end()?pathValue->second.TryGet<std::string>():nullptr;if(!path)return Result<LoadedUIScene>::Failure(SceneError("PXUI2613","Binding path must be a String",&record,targetName));const auto* property=TypeRegistry::Global().FindProperty(record.type,targetName);if(!property||!property->set)return Result<LoadedUIScene>::Failure(SceneError("PXUI2614","Binding target property does not exist: "+targetName,&record,targetName));std::string formatterName;if(const auto formatter=definition->find("formatter");formatter!=definition->end()){const auto* name=formatter->second.TryGet<std::string>();if(!name)return Result<LoadedUIScene>::Failure(SceneError("PXUI2615","Formatter must be a String",&record,targetName));formatterName=*name;}BindingTarget target{property->type,record.name+"."+targetName,[object,property](const Variant& bound){return property->set(*object,bound);}};auto binding=Binding::Create(*viewModel,*path,std::move(target),formatters,std::move(formatterName));if(!binding)return Result<LoadedUIScene>::Failure(binding.Diagnostics());loaded.bindings.push_back(std::move(binding.Value()));}}
        if(const auto found=record.properties.find("events");found!=record.properties.end()){const auto* definitions=found->second.AsObject();if(!definitions)return Result<LoadedUIScene>::Failure(SceneError("PXUI2617","events must be an Object",&record,"events"));for(const auto& [signal,value]:*definitions){const auto* definition=value.AsObject();if(!definition)return Result<LoadedUIScene>::Failure(SceneError("PXUI2617","Event binding must be an Object",&record,signal));const auto action=definition->find("action");const auto* command=action!=definition->end()?action->second.TryGet<std::string>():nullptr;if(!command||!ActionRegistry::Builtins().Find(*command))return Result<LoadedUIScene>::Failure(SceneError("PXUI2620","Unknown typed UI action",&record,signal));
            if (signal == "activated") {
                auto* button = dynamic_cast<Button*>(object);
                if (!button)
                    return Result<LoadedUIScene>::Failure(SceneError("PXUI2618", "The activated event requires a Button-compatible control", &record, signal));
                button->SetCommand(*command);
                continue;
            }
            return Result<LoadedUIScene>::Failure(SceneError("PXUI2619", "Unsupported UI event signal: " + signal, &record, signal));
        }}
    }
    return Result<LoadedUIScene>::Success(std::move(loaded));
}

Result<LoadedUIScene> InstantiateUIScene(const resource::TypedDocument& document, IViewModel* viewModel,
                                         const FormatterRegistry& formatters) {
    return InstantiateUIScene(document, viewModel, formatters, {});
}

Result<LoadedUIScene> LoadUIScene(const std::filesystem::path& path, IViewModel* viewModel,
                                  const FormatterRegistry& formatters) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        auto d = SceneError("PXUI2616", "Cannot open typed UI scene: " + path.string()); d.source.path = path.string();
        return Result<LoadedUIScene>::Failure(std::move(d));
    }
    std::ostringstream text; text << stream.rdbuf();
    auto document = resource::ParseTypedDocument(text.str(), path.string());
    if (!document) { for (const auto& d : document.Diagnostics()) diag::Emit(d); return Result<LoadedUIScene>::Failure(document.Diagnostics()); }
    const UIDocumentLoader loader=[&path](const ResourceRefValue& reference)->Result<resource::TypedDocument>{
        std::filesystem::path candidate=reference.lastKnownPath;
        if(candidate.is_relative()){
            auto parent=path.parent_path();
            while(!parent.empty()){
                const auto resolved=parent/candidate;
                if(std::filesystem::exists(resolved)){candidate=resolved;break;}
                const auto next=parent.parent_path();if(next==parent)break;parent=next;
            }
        }
        std::ifstream input(candidate,std::ios::binary);if(!input)return Result<resource::TypedDocument>::Failure(SceneError("PXUI2627","Referenced UI resource was not found: "+candidate.string()));
        std::ostringstream buffer;buffer<<input.rdbuf();return resource::ParseTypedDocument(buffer.str(),candidate.string());
    };
    return InstantiateUIScene(document.Value(), viewModel, formatters, loader);
}

}  // namespace px::ui
