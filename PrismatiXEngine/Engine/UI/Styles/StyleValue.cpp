#include "Engine/UI/Styles/StyleValue.h"

namespace px::ui {

StyleValue StyleValue::Unset() { return {}; }

StyleValue StyleValue::Literal(Variant value) {
    StyleValue result;
    result.m_kind = StyleValueKind::Literal;
    result.m_literal = std::move(value);
    return result;
}

StyleValue StyleValue::Token(TokenId id, std::string lastKnownName) {
    StyleValue result;
    result.m_kind = StyleValueKind::TokenReference;
    result.m_token = id;
    result.m_lastKnownTokenName = std::move(lastKnownName);
    return result;
}

StyleValue StyleValue::Clone() const {
    StyleValue result = *this;
    result.m_literal = m_literal.Clone();
    return result;
}

bool StyleValue::operator==(const StyleValue& other) const {
    return m_kind == other.m_kind && m_literal == other.m_literal && m_token == other.m_token &&
           m_lastKnownTokenName == other.m_lastKnownTokenName;
}

}  // namespace px::ui
