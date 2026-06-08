#pragma once

#include "Engine/Core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace px::vn {

struct DialogueState {
    std::string speaker;
    std::string fullText;
    std::string displayText;
    std::string voice;
    int currentChar = 0;
    int totalChars = 0;
    bool finished = false;
    Color textColor{ 245, 248, 255, 255 };
    Color outlineColor{ 0, 0, 0, 255 };
    std::string effect;
    float effectProgress = 0.0f;
};

class Dialogue {
public:
    void SetText(const std::string& speaker, const std::string& text, int speedMs, Color textColor,
                 Color outlineColor, const std::string& voice = "", const std::string& effect = "");
    void Update(std::uint64_t nowMs);
    void ShowAll();

    [[nodiscard]] const DialogueState& State() const { return m_state; }
    [[nodiscard]] bool Finished() const { return m_state.finished; }
    [[nodiscard]] bool Active() const { return !m_state.fullText.empty(); }
    void Clear() { m_state = DialogueState{}; }

private:
    DialogueState m_state;
    std::vector<std::string> m_glyphs;
    int m_speedMs = 30;
    std::uint64_t m_lastStepMs = 0;
};

}
