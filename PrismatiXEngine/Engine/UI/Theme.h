#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Core/Result.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Styles/ResolvedStyleCache.h"

#include <string>
#include <unordered_map>

namespace px::ui {

class Control;

struct StyleBox {
    Color background{28, 31, 40, 255};
    Color border{65, 72, 91, 255};
    float borderWidth = 1.0f;
    float cornerRadius = 6.0f;
    Vec2 padding{12.0f, 8.0f};
};

struct ControlStyle {
    StyleBox normal;
    StyleBox hover;
    StyleBox pressed;
    StyleBox disabled;
    StyleBox focused;
    Color text{236, 239, 244, 255};
    Color textDisabled{140, 145, 158, 255};
    std::string font;
    int fontSize = 24;
    float spacing = 8.0f;
};

class Theme {
public:
    Theme();

    void Set(std::string variant, ControlStyle style);
    [[nodiscard]] const ControlStyle& Resolve(std::string_view variant) const;
    [[nodiscard]] ControlStyle Resolve(const Control& control) const;
    [[nodiscard]] Result<ResolvedStyle> ResolveStyle(const StyleResolveRequest& request) const;
    [[nodiscard]] StyleThemeData& StyleData() { return m_styleData; }
    [[nodiscard]] const StyleThemeData& StyleData() const { return m_styleData; }
    Status SetStyleData(StyleThemeData data);
    [[nodiscard]] const StylePropertyRegistry& StyleProperties() const { return m_styleProperties; }
    void SetToken(std::string name, Variant value);
    [[nodiscard]] const Variant* FindToken(std::string_view name) const;
    [[nodiscard]] const std::unordered_map<std::string, Variant>& Tokens() const { return m_tokens; }
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }

private:
    std::unordered_map<std::string, ControlStyle> m_styles;
    std::unordered_map<std::string, Variant> m_tokens;
    std::uint64_t m_revision = 1;
    StyleThemeData m_styleData;
    StylePropertyRegistry m_styleProperties;
    StyleResolver m_styleResolver;
    mutable ResolvedStyleCache m_styleCache;
};

}  // namespace px::ui

namespace px::ui { [[nodiscard]] Result<Theme> LoadEmbeddedTheme(const resource::TypedDocument& document); }
