#include "Engine/VN/Runtime/Dialogue.h"

#include "Engine/Text/Typography.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

namespace px::vn {
namespace {

// Story reveal tags: `[w 500ms]` pauses before the next glyph, `[speed 50%]`
// ... `[/speed]` scales the reveal rate and `[nowait]` ... `[/nowait]` shows
// its text at once. They are consumed here and never reach the visible text,
// the backlog or saves.
struct RevealTag {
    enum class Kind { Wait, Speed, SpeedEnd, NoWait, NoWaitEnd };
    Kind kind = Kind::Wait;
    std::uint64_t waitMs = 0;
    int percent = 100;
    std::size_t length = 0;
};

std::optional<double> ParseNumber(const std::string_view value) {
    double number = 0.0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(number) || number < 0.0)
        return std::nullopt;
    return number;
}

std::optional<RevealTag> ReadRevealTag(const std::string_view text) {
    if (!text.starts_with('[')) return std::nullopt;
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    const std::string_view body = text.substr(1, close - 1);
    RevealTag tag;
    tag.length = close + 1;
    if (body == "/speed") { tag.kind = RevealTag::Kind::SpeedEnd; return tag; }
    if (body == "nowait") { tag.kind = RevealTag::Kind::NoWait; return tag; }
    if (body == "/nowait") { tag.kind = RevealTag::Kind::NoWaitEnd; return tag; }
    if (body.starts_with("w ")) {
        std::string_view value = body.substr(2);
        double scale = 1.0;
        if (value.ends_with("ms")) value.remove_suffix(2);
        else if (value.ends_with('s')) { value.remove_suffix(1); scale = 1000.0; }
        const auto amount = ParseNumber(value);
        if (!amount || *amount * scale > 60'000.0) return std::nullopt;
        tag.kind = RevealTag::Kind::Wait;
        tag.waitMs = static_cast<std::uint64_t>(std::lround(*amount * scale));
        return tag;
    }
    if (body.starts_with("speed ") && body.ends_with('%')) {
        const auto amount = ParseNumber(body.substr(6, body.size() - 7));
        if (!amount || *amount < 10.0 || *amount > 1000.0) return std::nullopt;
        tag.kind = RevealTag::Kind::Speed;
        tag.percent = static_cast<int>(std::lround(*amount));
        return tag;
    }
    return std::nullopt;
}

}  // namespace

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
    // Style tags take no typewriter step: an opening tag rides on the next
    // glyph and a closing tag on the previous one, so revealed text always
    // carries its style and never shows a half-typed tag.
    m_glyphs.clear();
    m_extraDelayMs.clear();
    m_speedPercent.clear();
    std::uint64_t carry = 0;
    std::string pendingTags;
    std::vector<int> speeds;
    int instant = 0;
    const auto push = [&](std::string glyph) {
        m_glyphs.push_back(std::move(pendingTags) + std::move(glyph));
        pendingTags.clear();
        m_extraDelayMs.push_back(carry);
        m_speedPercent.push_back(instant > 0 ? 0 : speeds.empty() ? 100 : speeds.back());
        carry = 0;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        if (const auto reveal = ReadRevealTag(std::string_view(text).substr(i))) {
            switch (reveal->kind) {
            case RevealTag::Kind::Wait: carry += reveal->waitMs; break;
            case RevealTag::Kind::Speed: speeds.push_back(reveal->percent); break;
            case RevealTag::Kind::SpeedEnd: if (!speeds.empty()) speeds.pop_back(); break;
            case RevealTag::Kind::NoWait: ++instant; break;
            case RevealTag::Kind::NoWaitEnd: if (instant > 0) --instant; break;
            }
            i += reveal->length;
            continue;
        }
        if (const std::size_t tag = text::StyleTagLength(std::string_view(text).substr(i))) {
            if (text[i + 1] == '/' && !m_glyphs.empty() && pendingTags.empty())
                m_glyphs.back() += text.substr(i, tag);
            else
                pendingTags += text.substr(i, tag);
            i += tag;
            continue;
        }
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
                push(text.substr(i, len));
                i += len;
                continue;
            }
        }
        if (text.compare(i, 4, "[br]") == 0) {
            push("[br]");
            i += 4;
            continue;
        }
        const std::string_view remaining(text.data() + i, text.size() - i);
        const auto boundaries = text::GraphemeBoundaries(remaining);
        const std::size_t len = boundaries.size() > 1 ? boundaries[1] : 1;
        push(text.substr(i, len));
        i += len;
    }
    // Tags after the last glyph (or a line of tags only) still belong to it.
    if (!pendingTags.empty()) {
        if (m_glyphs.empty()) push({});
        else m_glyphs.back() += pendingTags;
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
        // [speed N%] scales the step; [nowait] (0%) reveals without one.
        const int percent = idx < m_speedPercent.size() ? m_speedPercent[idx] : 100;
        const std::uint64_t scaled = percent <= 0 ? 0 : percent == 100 ? step
            : std::max<std::uint64_t>(1, (step * 100 + static_cast<std::uint64_t>(percent) / 2) /
                                             static_cast<std::uint64_t>(percent));
        const std::uint64_t need = scaled + (idx < m_extraDelayMs.size() ? m_extraDelayMs[idx] : 0);
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
    SetFace(previous.faceCharacter, previous.faceExpression);
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
