#include "Engine/UI/UIResourceResolver.h"

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

Result<ExpandedUIDocument> ExpandDocument(const resource::TypedDocument& source,
                                           const UIDocumentLoader& loader,
                                           std::unordered_set<Uuid, UuidHash>& stack) {
    ExpandedUIDocument output; output.document = source; output.document.nodes.clear();
    for (const auto& record : source.nodes) {
        if (record.type != "ComponentInstance") {
            output.document.nodes.push_back(record);
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
        auto expanded = ExpandDocument(loaded.Value(), loader, stack);
        stack.erase(reference->id);
        if (!expanded) return expanded;

        const resource::NodeRecord* sourceRoot = nullptr;
        for (const auto& node : expanded.Value().document.nodes)
            if (node.parent.Empty()) { if (sourceRoot) return Result<ExpandedUIDocument>::Failure(
                Error("PXUI3014", "UIComponent must have exactly one root", reference->lastKnownPath)); sourceRoot=&node; }
        if (!sourceRoot) return Result<ExpandedUIDocument>::Failure(
            Error("PXUI3014", "UIComponent root is missing", reference->lastKnownPath));

        std::unordered_map<Uuid, Uuid, UuidHash> ids;
        for (const auto& node : expanded.Value().document.nodes)
            ids[node.id] = node.id == sourceRoot->id ? record.id
                : Uuid::FromName(record.id.ToString() + "/" + node.id.ToString());
        const VariantObject* overrides = nullptr;
        if (const auto it = record.properties.find("overrides"); it != record.properties.end())
            overrides = it->second.AsObject();
        for (auto node : expanded.Value().document.nodes) {
            const Uuid sourceId = node.id;
            node.id = ids.at(sourceId);
            node.parent = sourceId == sourceRoot->id ? record.parent : ids.at(node.parent);
            if (sourceId == sourceRoot->id) {
                for (const auto& [name, value] : record.properties)
                    if (name != "component" && name != "overrides") node.properties[name] = value.Clone();
                node.name = record.name;
            }
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

Color ReadColor(const VariantObject& object, const char* key, Color fallback) {
    if (const auto it=object.find(key); it!=object.end()) if (const auto* v=it->second.TryGet<Color>()) return *v;
    return fallback;
}
double ReadNumber(const VariantObject& object, const char* key, double fallback) {
    if (const auto it=object.find(key); it!=object.end()) {
        if (const auto* v=it->second.TryGet<double>()) return *v;
        if (const auto* v=it->second.TryGet<std::int64_t>()) return static_cast<double>(*v);
    }
    return fallback;
}
}

Result<ExpandedUIDocument> ExpandUIComponents(const resource::TypedDocument& source,
                                               const UIDocumentLoader& loader) {
    std::unordered_set<Uuid, UuidHash> stack;
    return ExpandDocument(source, loader, stack);
}

Result<Variant> ResolveThemeValue(const Variant& value, const VariantObject& tokens) {
    std::unordered_set<std::string> resolving;
    return ResolveValue(value, tokens, resolving);
}

Result<Theme> LoadUITheme(const resource::TypedDocument& document) {
    if (document.kind != resource::DocumentKind::Resource || document.type != "UITheme")
        return Result<Theme>::Failure(Error("PXUI3020", "Theme resource must be @pxresource UITheme"));
    const auto tokenIt=document.properties.find("tokens"), styleIt=document.properties.find("styles");
    const VariantObject empty;
    const VariantObject* tokens=tokenIt==document.properties.end()?&empty:tokenIt->second.AsObject();
    const VariantObject* styles=styleIt==document.properties.end()?&empty:styleIt->second.AsObject();
    if(!tokens||!styles)return Result<Theme>::Failure(Error("PXUI3021","UITheme tokens and styles must be objects"));
    Theme theme;
    for(const auto& [name,value]:*tokens){auto resolved=ResolveThemeValue(value,*tokens);if(!resolved)return Result<Theme>::Failure(resolved.Diagnostics());theme.SetToken(name,resolved.TakeValue());}
    for(const auto& [name,value]:*styles){auto resolved=ResolveThemeValue(value,*tokens);if(!resolved)return Result<Theme>::Failure(resolved.Diagnostics());const auto* fields=resolved.Value().AsObject();if(!fields)continue;ControlStyle style=theme.Resolve(name);
        style.normal.background=ReadColor(*fields,"background",style.normal.background);style.normal.border=ReadColor(*fields,"border",style.normal.border);style.text=ReadColor(*fields,"text",style.text);
        style.normal.cornerRadius=static_cast<float>(ReadNumber(*fields,"cornerRadius",style.normal.cornerRadius));style.fontSize=static_cast<int>(ReadNumber(*fields,"fontSize",style.fontSize));
        if(const auto it=fields->find("padding");it!=fields->end())if(const auto* paddingValue=it->second.TryGet<Vec2>())style.normal.padding=*paddingValue;
        if(const auto it=fields->find("font");it!=fields->end()){if(const auto* fontPath=it->second.TryGet<std::string>())style.font=*fontPath;else if(const auto* fontResource=it->second.TryGet<ResourceRefValue>())style.font=fontResource->lastKnownPath;}
        style.hover=style.normal;style.pressed=style.normal;style.disabled=style.normal;style.focused=style.normal;theme.Set(name,std::move(style));}
    return Result<Theme>::Success(std::move(theme));
}

}  // namespace px::ui
