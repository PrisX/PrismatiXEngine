#include "Engine/Accessibility/SpeechService.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>
#endif

namespace px::accessibility {

SpeechService::SpeechService() {
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(initialized);
    ISpVoice* voice = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                   IID_ISpVoice, reinterpret_cast<void**>(&voice))))
        m_voice = voice;
#endif
}

SpeechService::~SpeechService() {
    Stop();
#ifdef _WIN32
    if (m_voice) static_cast<ISpVoice*>(m_voice)->Release();
    if (m_comInitialized) CoUninitialize();
#endif
}

void SpeechService::Speak(const std::string& utf8Text, const bool interrupt) {
#ifdef _WIN32
    if (!m_voice || utf8Text.empty()) return;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.c_str(),
                                          static_cast<int>(utf8Text.size()), nullptr, 0);
    if (count <= 0) return;
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.c_str(),
                        static_cast<int>(utf8Text.size()), wide.data(), count);
    const DWORD flags = SPF_ASYNC | (interrupt ? SPF_PURGEBEFORESPEAK : 0);
    static_cast<ISpVoice*>(m_voice)->Speak(wide.c_str(), flags, nullptr);
#else
    (void)utf8Text; (void)interrupt;
#endif
}

void SpeechService::Stop() {
#ifdef _WIN32
    if (m_voice) static_cast<ISpVoice*>(m_voice)->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
#endif
}

}  // namespace px::accessibility
