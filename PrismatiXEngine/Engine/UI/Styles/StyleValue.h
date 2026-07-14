#pragma once

#include "Engine/Core/Variant.h"
#include "Engine/UI/Styles/StyleTypes.h"

#include <string>

namespace px::ui {

enum class StyleValueKind : std::uint8_t { Unset, Literal, TokenReference };

class StyleValue {
public:
    StyleValue() = default;

    [[nodiscard]] static StyleValue Unset();
    [[nodiscard]] static StyleValue Literal(Variant value);
    [[nodiscard]] static StyleValue Token(TokenId id, std::string lastKnownName = {});

    [[nodiscard]] StyleValueKind Kind() const { return m_kind; }
    [[nodiscard]] bool IsUnset() const { return m_kind == StyleValueKind::Unset; }
    [[nodiscard]] bool IsLiteral() const { return m_kind == StyleValueKind::Literal; }
    [[nodiscard]] bool IsTokenReference() const {
        return m_kind == StyleValueKind::TokenReference;
    }
    [[nodiscard]] const Variant& LiteralValue() const { return m_literal; }
    [[nodiscard]] const TokenId& TokenReference() const { return m_token; }
    [[nodiscard]] const std::string& LastKnownTokenName() const { return m_lastKnownTokenName; }

    [[nodiscard]] StyleValue Clone() const;
    [[nodiscard]] bool operator==(const StyleValue& other) const;

private:
    StyleValueKind m_kind = StyleValueKind::Unset;
    Variant m_literal;
    TokenId m_token;
    std::string m_lastKnownTokenName;
};

}  // namespace px::ui
