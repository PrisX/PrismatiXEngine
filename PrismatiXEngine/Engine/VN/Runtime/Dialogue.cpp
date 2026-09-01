#include "Engine/VN/Runtime/Dialogue.h"

#include "Engine/Text/Typography.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace px::vn {

void Dialogue::SetText(const std::string& speaker, const std::string& text, int speedMs,
                       Color textColor, Color outlineColor, const std::string& voice,
                       const std::string& effect) {
    m_state = DialogueState{};
    m_state.speaker = speaker;
    m_state.voice = voice;
    m_state.textColor = textColor;
    m_state.outlineColor = outlineColor;
    m_state.effect = effect;

    // Split into observable grapheme/rich-text tokens, consuming inline
    // {w=ms} pauses. Ruby markup is one atomic token so an incomplete tag is
    // never exposed by the typewriter and then interpreted as visible text.
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
        if (text.compare(i, 6, "[ruby=") == 0) {
            const std::size_t header = text.find(']', i + 6);
            const std::size_t close = header == std::string::npos
                                          ? std::string::npos
                                          : text.find("[/ruby]", header + 1);
            if (close != std::string::npos) {
                const std::size_t len = close + 7 - i;
                m_glyphs.push_back(text.substr(i, len));
                m_extraDelayMs.push_back(carry);
                carry = 0;
                i += len;
                continue;
            }
        }
        if (text.compare(i, 4, "[br]") == 0) {
            m_glyphs.emplace_back("[br]");
            m_extraDelayMs.push_back(carry);
            carry = 0;
            i += 4;
            continue;
        }
        const std::string_view remaining(text.data() + i, text.size() - i);
        const auto boundaries = text::GraphemeBoundaries(remaining);
        const std::size_t len = boundaries.size() > 1 ? boundaries[1] : 1;
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

void Dialogue::Relocalize(const std::string& speaker, const std::string& text) {
    const DialogueState previous = m_state;
    const int previousTotal = std::max(1, previous.totalChars);
    const float progress = previous.finished
                               ? 1.0f
                               : std::clamp(
                                     static_cast<float>(previous.currentChar) /
                                         static_cast<float>(previousTotal),
                                     0.0f, 1.0f);
    SetText(speaker, text, m_speedMs, previous.textColor,
            previous.outlineColor, previous.voice, previous.effect);
    m_state.effectProgress = previous.effectProgress;
    const int visible = previous.finished
                            ? m_state.totalChars
                            : std::clamp(static_cast<int>(
                                             std::lround(progress * m_state.totalChars)),
                                         0, m_state.totalChars);
    m_state.displayText.clear();
    for (int index = 0; index < visible; ++index)
        m_state.displayText += m_glyphs[static_cast<std::size_t>(index)];
    m_state.currentChar = visible;
    m_state.finished = visible >= m_state.totalChars;
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
