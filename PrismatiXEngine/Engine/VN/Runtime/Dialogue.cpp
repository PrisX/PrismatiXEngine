#include "Engine/VN/Runtime/Dialogue.h"

#include <algorithm>

namespace px::vn {

namespace {
std::vector<std::string> SplitUtf8(const std::string& text) {
    std::vector<std::string> glyphs;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > text.size()) len = text.size() - i;
        glyphs.push_back(text.substr(i, len));
        i += len;
    }
    return glyphs;
}
}

void Dialogue::SetText(const std::string& speaker, const std::string& text, int speedMs,
                       Color textColor, Color outlineColor, const std::string& voice,
                       const std::string& effect) {
    m_state = DialogueState{};
    m_state.speaker = speaker;
    m_state.fullText = text;
    m_state.voice = voice;
    m_state.textColor = textColor;
    m_state.outlineColor = outlineColor;
    m_state.effect = effect;
    m_glyphs = SplitUtf8(text);
    m_state.totalChars = static_cast<int>(m_glyphs.size());
    m_speedMs = speedMs;
    m_lastStepMs = 0;

    if (speedMs <= 0) {
        ShowAll();
    }
}

void Dialogue::Update(std::uint64_t nowMs) {
    if (m_state.finished) {
        return;
    }
    if (m_lastStepMs == 0) {
        m_lastStepMs = nowMs;
    }
    const std::uint64_t step = static_cast<std::uint64_t>(std::max(1, m_speedMs));
    while (m_state.currentChar < m_state.totalChars && nowMs - m_lastStepMs >= step) {
        m_state.displayText += m_glyphs[m_state.currentChar];
        ++m_state.currentChar;
        m_lastStepMs += step;
    }
    if (m_state.currentChar >= m_state.totalChars) {
        m_state.finished = true;
    }
}

void Dialogue::ShowAll() {
    m_state.displayText = m_state.fullText;
    m_state.currentChar = m_state.totalChars;
    m_state.finished = true;
}

}
