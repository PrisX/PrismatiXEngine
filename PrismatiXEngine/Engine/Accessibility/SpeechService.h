#pragma once

#include <string>

namespace px::accessibility {

class SpeechService {
public:
    SpeechService();
    ~SpeechService();
    SpeechService(const SpeechService&) = delete;
    SpeechService& operator=(const SpeechService&) = delete;

    [[nodiscard]] bool Available() const { return m_voice != nullptr; }
    void Speak(const std::string& utf8Text, bool interrupt = true);
    void Stop();

private:
    void* m_voice = nullptr;
    bool m_comInitialized = false;
};

}  // namespace px::accessibility
