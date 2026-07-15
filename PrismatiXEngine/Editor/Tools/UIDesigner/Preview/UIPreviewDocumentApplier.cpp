#include "Editor/Tools/UIDesigner/Preview/UIPreviewDocumentApplier.h"

#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"

#include <algorithm>

namespace px::editor {
namespace {

Variant PreviewDefault(const VariantType type) {
    switch (type) {
        case VariantType::Bool: return Variant(false);
        case VariantType::Integer: return Variant(std::int64_t{0});
        case VariantType::Number: return Variant(0.0);
        case VariantType::String: return Variant(std::string("Preview"));
        case VariantType::Vec2: return Variant(Vec2{});
        case VariantType::Rect: return Variant(Rect{});
        case VariantType::Color: return Variant(Color{});
        default: return Variant{};
    }
}

void EmitDiagnostics(const Status& status) {
    for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
}

}  // namespace

UIPreviewDocumentApplier::UIPreviewDocumentApplier(ui::UIContext& context,
                                                   ui::UIDocumentLoader loader)
    : m_context(context), m_loader(std::move(loader)) {}

bool UIPreviewDocumentApplier::Load(const resource::TypedDocument& document,
                                    const std::string& sourcePath) {
    m_bindings.clear();
    (void)ui::RegisterBuiltinUITypes();
    m_viewModel = ui::ObservableViewModel{};
    for (const auto& node : document.nodes) {
        const auto bindings = node.properties.find("bindings");
        if (bindings == node.properties.end()) continue;
        const auto* definitions = bindings->second.AsObject();
        if (!definitions) continue;
        for (const auto& [targetName, value] : *definitions) {
            const auto* definition = value.AsObject();
            if (!definition) continue;
            const auto pathValue = definition->find("path");
            const auto* bindingPath =
                pathValue != definition->end()
                    ? pathValue->second.TryGet<std::string>()
                    : nullptr;
            if (!bindingPath || m_viewModel.Describe(*bindingPath)) continue;
            const auto* property = TypeRegistry::Global().FindProperty(node.type, targetName);
            if (!property) continue;
            VariantType sourceType = property->type;
            if (const auto format = definition->find("formatter");
                format != definition->end())
                if (const auto* name = format->second.TryGet<std::string>())
                    if (const auto* formatter = m_context.Formatters().Find(*name))
                        sourceType = formatter->input;
            m_viewModel.Define(*bindingPath, PreviewDefault(sourceType), true);
        }
    }

    auto loaded = ui::InstantiateUIScene(document, &m_viewModel,
                                         m_context.Formatters(), m_loader);
    if (!loaded) {
        for (const auto& diagnostic : loaded.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    auto animations = std::move(loaded.Value().animations);
    auto theme = std::move(loaded.Value().theme);
    auto triggers = std::move(loaded.Value().triggers);
    auto interaction = std::move(loaded.Value().interactionGraph);
    m_bindings = std::move(loaded.Value().bindings);
    const Status rootStatus = m_context.SetRoot(std::move(loaded.Value().root));
    if (!rootStatus) {
        EmitDiagnostics(rootStatus);
        return false;
    }
    if (theme) m_context.SetTheme(std::move(*theme));
    if (animations) {
        const Status status = m_context.SetAnimations(std::move(*animations), true);
        if (!status) {
            EmitDiagnostics(status);
            return false;
        }
    }
    const Status triggerStatus = m_context.ConfigureTriggers(
        std::move(triggers), std::move(interaction), sourcePath);
    if (!triggerStatus) {
        EmitDiagnostics(triggerStatus);
        return false;
    }
    m_lastDocument = document;
    return true;
}

bool UIPreviewDocumentApplier::Apply(const resource::TypedDocument& document,
                                     const std::string& sourcePath,
                                     const DocumentChangeSet& changes) {
    const auto rebuild = [&] {
        (void)Load(document, sourcePath);
        return false;
    };
    const PreviewUpdate planned = PlanPreviewUpdate(changes, document.id);
    if (!m_lastDocument || !m_context.Root() ||
        HasPreviewUpdate(planned, PreviewUpdate::RebuildScene) ||
        m_lastDocument->id != document.id ||
        m_lastDocument->nodes.size() != document.nodes.size())
        return rebuild();
    if (HasPreviewUpdate(planned, PreviewUpdate::ReconnectBindings) ||
        changes.properties.empty())
        return rebuild();

    for (const auto& changed : changes.properties) {
        if (changed.node == document.id) {
            if (changed.property == "animations") {
                const auto found = document.properties.find("animations");
                ui::UIAnimationLibrary library;
                if (found != document.properties.end()) {
                    auto parsed = ui::ParseUIAnimationLibrary(found->second, sourcePath);
                    if (!parsed) return rebuild();
                    library = parsed.TakeValue();
                }
                if (!m_context.SetAnimations(std::move(library), true)) return rebuild();
                continue;
            }
            if (changed.property == "styleSystem" || changed.property == "theme") {
                ui::Theme theme;
                const auto found = document.properties.find("styleSystem");
                if (found != document.properties.end()) {
                    auto parsed = ui::ParseStyleTheme(found->second);
                    if (!parsed || !theme.SetStyleData(parsed.TakeValue())) return rebuild();
                }
                m_context.SetTheme(std::move(theme));
                continue;
            }
            return rebuild();
        }

        const auto after = std::find_if(document.nodes.begin(), document.nodes.end(),
                                        [&](const auto& node) { return node.id == changed.node; });
        const auto before = std::find_if(m_lastDocument->nodes.begin(),
                                         m_lastDocument->nodes.end(),
                                         [&](const auto& node) { return node.id == changed.node; });
        if (after == document.nodes.end() || before == m_lastDocument->nodes.end() ||
            after->parent != before->parent || after->type != before->type)
            return rebuild();
        auto* object = dynamic_cast<ui::Control*>(m_context.Root()->Find(changed.node));
        if (!object) return rebuild();

        if (changed.property == "$name") {
            object->SetName(after->name);
            continue;
        }
        if (changed.property == "bindings" || changed.property == "triggers" ||
            changed.property == "componentProperties" ||
            changed.property == "componentEvents" ||
            changed.property == "componentSlot" || changed.property == "overrides")
            return rebuild();
        if (changed.property == "styleBinding") {
            const auto value = after->properties.find("styleBinding");
            if (value == after->properties.end()) object->SetStyleBinding({});
            else {
                auto parsed = ui::ParseStyleBinding(value->second);
                if (!parsed) return rebuild();
                object->SetStyleBinding(parsed.TakeValue());
            }
            object->InvalidateLayout();
            continue;
        }

        const auto* property = TypeRegistry::Global().FindProperty(after->type,
                                                                   changed.property);
        if (!property || !property->set) return rebuild();
        const auto value = after->properties.find(changed.property);
        const Variant& applied = value == after->properties.end()
                                     ? property->defaultValue
                                     : value->second;
        if (!property->set(*object, applied)) return rebuild();
    }

    m_lastDocument = document;
    return true;
}

ui::Control* UIPreviewDocumentApplier::Find(const Uuid& id) const {
    return m_context.Root()
               ? dynamic_cast<ui::Control*>(m_context.Root()->Find(id))
               : nullptr;
}

}  // namespace px::editor
