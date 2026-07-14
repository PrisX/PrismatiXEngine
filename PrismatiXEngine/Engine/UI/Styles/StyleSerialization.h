#pragma once

#include "Engine/UI/Styles/StyleDefinition.h"

namespace px::ui {
[[nodiscard]] Result<ControlStyleBinding> ParseStyleBinding(const Variant& value);
[[nodiscard]] Variant WriteStyleBinding(const ControlStyleBinding& binding);
[[nodiscard]] Result<StyleThemeData> ParseStyleTheme(const Variant& value);
[[nodiscard]] Variant WriteStyleTheme(const StyleThemeData& theme);
}
