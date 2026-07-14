#include "Engine/UI/Styles/StyleTypes.h"

namespace px::ui {

const char* StyleLayerName(StyleLayerKind layer) {
    switch (layer) {
        case StyleLayerKind::ThemeGlobalDefaults: return "Theme Global Defaults";
        case StyleLayerKind::ControlTypeDefaults: return "Control Type Defaults";
        case StyleLayerKind::BaseStyle: return "Base Style";
        case StyleLayerKind::VariantAxis: return "Variant Axis";
        case StyleLayerKind::AppliedStyle: return "Applied Style";
        case StyleLayerKind::ActiveState: return "Active State";
        case StyleLayerKind::ComponentInstanceOverride: return "Component Instance Override";
        case StyleLayerKind::LocalControlOverride: return "Local Control Override";
    }
    return "Style";
}

const char* StyleStateName(StyleState state) {
    switch (state) {
        case StyleState::Normal: return "Normal";
        case StyleState::Hover: return "Hover";
        case StyleState::Pressed: return "Pressed";
        case StyleState::Focused: return "Focused";
        case StyleState::Disabled: return "Disabled";
        case StyleState::Checked: return "Checked";
        case StyleState::Selected: return "Selected";
    }
    return "Normal";
}

TokenId StableLegacyTokenId(std::string_view name) {
    return Uuid::FromName(std::string("prismatix.style.token.legacy/") + std::string(name));
}

StyleId StableLegacyStyleId(std::string_view name) {
    return Uuid::FromName(std::string("prismatix.style.definition.legacy/") + std::string(name));
}

}  // namespace px::ui
