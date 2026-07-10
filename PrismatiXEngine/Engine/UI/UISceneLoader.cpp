#include "Engine/UI/UISceneLoader.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UITypeRegistry.h"

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
                                         const FormatterRegistry& formatters) {
    const Status registration = RegisterBuiltinUITypes();
    if (!registration) return Result<LoadedUIScene>::Failure(registration.Diagnostics());
    if (document.kind != resource::DocumentKind::Scene)
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2603", "UI loader requires an @pxscene document"));
    if (document.nodes.empty())
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2604", "UI scene has no nodes"));

    std::unordered_map<Uuid, const resource::NodeRecord*, UuidHash> records;
    const resource::NodeRecord* rootRecord = nullptr;
    for (const auto& record : document.nodes) {
        if (!records.emplace(record.id, &record).second)
            return Result<LoadedUIScene>::Failure(SceneError("PXUI2605", "Duplicate node UUID in UI scene", &record));
        if (record.parent.Empty()) {
            if (rootRecord) return Result<LoadedUIScene>::Failure(SceneError("PXUI2606", "UI scene must have exactly one root Control", &record));
            rootRecord = &record;
        }
    }
    if (!rootRecord) return Result<LoadedUIScene>::Failure(SceneError("PXUI2607", "UI scene root is missing"));
    for (const auto& record : document.nodes) if (!record.parent.Empty() && !records.contains(record.parent))
        return Result<LoadedUIScene>::Failure(SceneError("PXUI2608", "UI node references a missing parent", &record));

    std::unordered_set<Uuid, UuidHash> building;
    std::function<Result<std::unique_ptr<Control>>(const resource::NodeRecord&)> build =
        [&](const resource::NodeRecord& record) -> Result<std::unique_ptr<Control>> {
            if (!building.insert(record.id).second)
                return Result<std::unique_ptr<Control>>::Failure(SceneError("PXUI2609", "Cycle detected in UI scene tree", &record));
            auto created = CreateControl(record); if (!created) return created;
            for (const auto& candidate : document.nodes) if (candidate.parent == record.id) {
                auto child = build(candidate); if (!child) return child;
                const Status added = created.Value()->AddChild(std::move(child.Value()));
                if (!added) return Result<std::unique_ptr<Control>>::Failure(added.Diagnostics());
            }
            building.erase(record.id); return created;
        };
    auto root = build(*rootRecord); if (!root) return Result<LoadedUIScene>::Failure(root.Diagnostics());

    LoadedUIScene loaded; loaded.root = std::move(root.Value());
    if(document.properties.contains("theme.styles")){auto theme=LoadEmbeddedTheme(document);if(!theme)return Result<LoadedUIScene>::Failure(theme.Diagnostics());loaded.theme=std::move(theme.Value());}
    if(document.properties.contains("animation.tracks")){auto animation=LoadEmbeddedAnimation(document);if(!animation)return Result<LoadedUIScene>::Failure(animation.Diagnostics());loaded.animation=std::move(animation.Value());}
    for (const auto& record : document.nodes) {
        auto* object = loaded.root->Find(record.id);
        if (!object) return Result<LoadedUIScene>::Failure(SceneError("PXUI2610", "Instantiated UI node could not be resolved", &record));
        for (const auto& [propertyName, value] : record.properties) {
            if (propertyName.starts_with("bind.") || propertyName.starts_with("formatter.")) continue;
            const auto* property = TypeRegistry::Global().FindProperty(record.type, propertyName);
            if (!property || !property->set)
                return Result<LoadedUIScene>::Failure(SceneError("PXUI2611", "Unknown UI property " + record.type + "." + propertyName, &record, propertyName));
            const Status set = property->set(*object, value);
            if (!set) return Result<LoadedUIScene>::Failure(set.Diagnostics());
        }
        for (const auto& [bindingName, pathValue] : record.properties) {
            if (!bindingName.starts_with("bind.")) continue;
            if (!viewModel) return Result<LoadedUIScene>::Failure(SceneError("PXUI2612", "Scene contains bindings but no ViewModel was provided", &record, bindingName));
            const auto* path = pathValue.TryGet<std::string>();
            if (!path) return Result<LoadedUIScene>::Failure(SceneError("PXUI2613", "Binding path must be a String", &record, bindingName));
            const std::string targetName = bindingName.substr(5);
            const auto* property = TypeRegistry::Global().FindProperty(record.type, targetName);
            if (!property || !property->set)
                return Result<LoadedUIScene>::Failure(SceneError("PXUI2614", "Binding target property does not exist: " + targetName, &record, bindingName));
            std::string formatterName;
            if (const auto it = record.properties.find("formatter." + targetName); it != record.properties.end()) {
                if (const auto* formatterText = it->second.TryGet<std::string>()) formatterName = *formatterText;
                else return Result<LoadedUIScene>::Failure(SceneError("PXUI2615", "Formatter name must be a String", &record, targetName));
            }
            BindingTarget target{property->type, record.name + "." + targetName,
                [object, property](const Variant& boundValue) { return property->set(*object, boundValue); }};
            auto binding = Binding::Create(*viewModel, *path, std::move(target), formatters, std::move(formatterName));
            if (!binding) return Result<LoadedUIScene>::Failure(binding.Diagnostics());
            loaded.bindings.push_back(std::move(binding.Value()));
        }
    }
    return Result<LoadedUIScene>::Success(std::move(loaded));
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
    return InstantiateUIScene(document.Value(), viewModel, formatters);
}

}  // namespace px::ui
