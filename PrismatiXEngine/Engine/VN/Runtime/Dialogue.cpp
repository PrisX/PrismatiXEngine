#include "Engine/VN/Runtime/Dialogue.h"

#include <algorithm>
#include <charconv>

namespace px::vn {

namespace {
std::size_t Utf8Len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
}

void Dialogue::SetText(const std::string& speaker, const std::string& text, int speedMs,
                       Color textColor, Color outlineColor, const std::string& voice,
                       const std::string& effect) {
    m_state = DialogueState{};
    m_state.speaker = speaker;
    m_state.voice = voice;
    m_state.textColor = textColor;
    m_state.outlineColor = outlineColor;
    m_state.effect = effect;

    // Split into glyphs, consuming inline {w=ms} pause tags: the tag adds an
    // extra typing delay before the following glyph and is stripped from the
    // displayed text.
    m_glyphs.clear();
    m_extraDelayMs.clear();
    std::uint64_t carry = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            if (close != std::string::npos && close > i + 3 &&
                text.compare(i + 1, 2, "w=") == 0) {
                int ms = 0;
                std::from_chars(text.data() + i + 3, text.data() + close, ms);
                carry += static_cast<std::uint64_t>(std::max(0, ms));
                i = close + 1;
                continue;
            }
        }
        std::size_t len = Utf8Len(static_cast<unsigned char>(text[i]));
        if (i + len > text.size()) len = text.size() - i;
        m_glyphs.push_back(text.substr(i, len));
        m_extraDelayMs.push_back(carry);
        carry = 0;
        i += len;
    }

    for (const std::string& glyph : m_glyphs) {
        m_state.fullText += glyph;
    }
    m_state.totalChars = static_cast<int>(m_glyphs.size());
    m_speedMs = speedMs;
    m_lastStepMs = 0;
    m_effectStartMs = 0;

    if (speedMs <= 0) {
        ShowAll();
    }
}

void Dialogue::Update(std::uint64_t nowMs) {
    if (m_effectStartMs == 0) m_effectStartMs = nowMs;
    m_state.effectProgress = static_cast<float>(nowMs - m_effectStartMs) / 1000.0f;
    if (m_state.finished) {
        return;
    }
    if (m_lastStepMs == 0) {
        m_lastStepMs = nowMs;
    }
    const std::uint64_t step = static_cast<std::uint64_t>(std::max(1, m_speedMs));
    while (m_state.currentChar < m_state.totalChars) {
        const std::size_t idx = static_cast<std::size_t>(m_state.currentChar);
        const std::uint64_t need = step + (idx < m_extraDelayMs.size() ? m_extraDelayMs[idx] : 0);
        if (nowMs - m_lastStepMs < need) {
            break;
        }
        m_state.displayText += m_glyphs[idx];
        ++m_state.currentChar;
        m_lastStepMs += need;
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

void Dialogue::RestoreState(const DialogueSnapshot& snapshot) {
    // Rebuild glyph boundaries through the normal parser, then restore the
    // exact visible/typewriter state captured by the session.
    SetText(snapshot.state.speaker, snapshot.state.fullText, std::max(1, snapshot.speedMs),
            snapshot.state.textColor, snapshot.state.outlineColor, snapshot.state.voice,
            snapshot.state.effect);
    m_state = snapshot.state;
    m_speedMs = snapshot.speedMs;
    m_lastStepMs = 0;
    m_effectStartMs = 0;
}

}
